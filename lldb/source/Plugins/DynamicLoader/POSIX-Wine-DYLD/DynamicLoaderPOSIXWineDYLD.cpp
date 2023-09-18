//===-- DynamicLoaderPOSIXWineDYLD.cpp ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DynamicLoaderPOSIXWineDYLD.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Section.h"
#include "lldb/Interpreter/OptionValueProperties.h"
#include "lldb/Interpreter/Property.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/MemoryRegionInfo.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadPlanStepInstruction.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/StringExtractor.h"

#include "Plugins/Process/Utility/LinuxProcMaps.h"

#include "llvm/ADT/Triple.h"
#include "llvm/Support/SHA1.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE_ADV(DynamicLoaderPOSIXWineDYLD, DynamicLoaderPosixWineDYLD)

namespace {
const char *g_wine_preloader_filename = "wine-preloader";
const char *g_wine_filename = "wine";
const char *g_wine_preloader_load_symbol = "wld_start";

const char *g_ntdll_filename = "ntdll.so";
const char *g_ntdll_load_symbol = "map_image_into_view";

const char *g_ld_filename_prefix = "ld-";
const char *g_ld_load_symbol = "_dl_debug_state";

#define LLDB_PROPERTIES_posixwinedyld
#include "DynamicLoaderPOSIXWineDYLDProperties.inc"

enum {
#define LLDB_PROPERTIES_posixwinedyld
#include "DynamicLoaderPOSIXWineDYLDPropertiesEnum.inc"
};

class PluginProperties : public Properties {
public:
  static ConstString &GetSettingName() {
    static ConstString g_plugin_name(DynamicLoaderPOSIXWineDYLD::GetPluginNameStatic());
    return g_plugin_name;
  }

  PluginProperties() : Properties() {
    m_collection_sp = std::make_shared<OptionValueProperties>(GetSettingName());
    m_collection_sp->Initialize(g_posixwinedyld_properties);
  }

  ~PluginProperties() override {}

  llvm::StringRef GetRemoteObjdumpPath() const {
    const uint32_t idx = ePropertyRemoteObjdumpPath;
    return m_collection_sp->GetPropertyAtIndexAsString(nullptr, idx, "objdump");
  }

  bool GetUseWineDynamicLoader() const {
    const uint32_t idx = ePropertyUseWineDynamicLoader;
    return m_collection_sp->GetPropertyAtIndexAsBoolean(nullptr, idx, true);
  }
};

typedef std::shared_ptr<PluginProperties> ProcessKDPPropertiesSP;

static const ProcessKDPPropertiesSP &GetGlobalPluginProperties() {
  static ProcessKDPPropertiesSP g_settings_sp;
  if (!g_settings_sp)
    g_settings_sp = std::make_shared<PluginProperties>();
  return g_settings_sp;
}

} // namespace

DynamicLoaderPOSIXWineDYLD::DynamicLoaderPOSIXWineDYLD(Process *process)
    : DynamicLoaderPOSIXDYLD(process) {}

DynamicLoaderPOSIXWineDYLD::~DynamicLoaderPOSIXWineDYLD() {}

void DynamicLoaderPOSIXWineDYLD::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                GetPluginDescriptionStatic(), CreateInstance,
                                DebuggerInitialize);
}

void DynamicLoaderPOSIXWineDYLD::DebuggerInitialize(Debugger &debugger) {
  if (!PluginManager::GetSettingForProcessPlugin(
          debugger, PluginProperties::GetSettingName())) {
    const bool is_global_setting = true;
    PluginManager::CreateSettingForProcessPlugin(
        debugger, GetGlobalPluginProperties()->GetValueProperties(),
        ConstString("Properties for the Wine/POSIX dynamic loader plug-in."),
        is_global_setting);
  }
}

void DynamicLoaderPOSIXWineDYLD::Terminate() {}

llvm::StringRef DynamicLoaderPOSIXWineDYLD::GetPluginNameStatic() {
  return "wine-dyld";
}

llvm::StringRef DynamicLoaderPOSIXWineDYLD::GetPluginDescriptionStatic() {
  return "Dynamic loader plug-in that watches for shared library "
         "loads in POSIX processes, with support for Wine.";
}

DynamicLoader *DynamicLoaderPOSIXWineDYLD::CreateInstance(Process *process,
                                                          bool force) {
  if (!GetGlobalPluginProperties()->GetUseWineDynamicLoader())
    return nullptr;

  bool should_create = force;
  if (!should_create) {
    const llvm::Triple &triple_ref =
        process->GetTarget().GetArchitecture().GetTriple();
    if (triple_ref.getOS() == llvm::Triple::Linux) {
      should_create = true;
    }
  }

  if (should_create)
    return new DynamicLoaderPOSIXWineDYLD(process);

  return nullptr;
}

llvm::StringRef DynamicLoaderPOSIXWineDYLD::GetPluginName() {
  return GetPluginNameStatic();
}

void DynamicLoaderPOSIXWineDYLD::DidAttach() {
  DynamicLoaderPOSIXDYLD::DidAttach();

  lldb::ModuleSP executable = GetTargetExecutable();
  if (!executable)
    return;

  ConstString name = executable->GetFileSpec().GetFilename();
  if (name == g_wine_preloader_filename || name == g_wine_filename) {
    LoadModulesFromMaps();
  }
}

namespace {

// Extracts build id from the contents of the .note.gnu.build-id
// section, as printed by llvm-objdump.
UUID ExtractBuildIdFromSectionHex(llvm::StringRef lines) {
  std::string result;
  UUID uuid;
  llvm::StringRef line;

  // Skip the header line.
  std::tie(line, lines) = lines.split('\n');

  while (!lines.empty()) {
    std::tie(line, lines) = lines.split('\n');

    StringExtractor extractor(line);
    extractor.SkipSpaces();
    // Skip the offset.
    extractor.GetU64(0, 16);
    // Skip the space.
    char space = extractor.GetChar();
    if (space != ' ') {
      break;
    }
    // Keep reading hex sequences until we get two spaces.
    char c = extractor.GetChar();
    while (true) {
      // Let us read the hex digits.
      for (; llvm::isHexDigit(c); c = extractor.GetChar()) {
        result.push_back(llvm::toLower(c));
      }
      // There should be a space after the hex digits.
      if (c == ' ') {
        c = extractor.GetChar();
        // If we see two spaces, we are finished with this line.
        if (c == ' ')
          break;
      } else {
        // If we did not get any space after the hex digits, we have a problem.
        // Just return an empty build ID.
        return uuid;
      }
    }
  }

  uuid.SetFromStringRef(result);

  return uuid;
}

// Tries to extract file-format id from a line of llvm-objdump's output.
llvm::Optional<llvm::StringRef> TryExtractFileFormat(llvm::StringRef line) {
  llvm::StringRef prefix;
  llvm::StringRef file_format;
  std::tie(prefix, file_format) = line.split(':');
  if (file_format.empty())
    return {};

  file_format = file_format.split("file format ").second;
  if (file_format.empty())
    return {};

  return file_format;
}

struct FileFormatAndUUID {
  std::string m_triple;
  UUID m_uuid;
};

// Reads and parses file format and build id of an executable
// on the debug target machine.
FileFormatAndUUID GetFileFormatAndUUID(PlatformSP platform,
                                       llvm::StringRef name) {

  llvm::SHA1 hasher;
  hasher.update(name);
  UUID uuid;
  uuid.SetFromStringRef(hasher.final());

  if (name.endswith(".dll") && (name.contains("system32") || name.contains("aarch64")))
    return {"aarch64-pc-windows-msvc", uuid};
  else if (name.contains(".so"))
    return {"aarch64-unknown-linux", uuid};
  else
    return {"", {}};
}

} // namespace

ModuleSP DynamicLoaderPOSIXWineDYLD::TryLoadModule(ModuleList &modules,
                                                   ConstString name,
                                                   lldb::addr_t address) {
  Target &target = m_process->GetTarget();
  PlatformSP platform = target.GetPlatform();

  // Let's try to find the module ourselves in the current list of modules.
  // We need to specify the platform file spec so that the module list
  // can match the modules.
  FileSpec file_spec(name.GetStringRef(), FileSpec::Style::posix);
  ModuleSpec spec(file_spec);
  spec.GetPlatformFileSpec() = file_spec;
  ModuleSP module_sp = modules.FindFirstModule(spec);

      Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_DYNAMIC_LOADER));
  if (module_sp)
    return module_sp;

  if (!module_sp) {
    // TODO Instead of calling objdump, we should do a quick pruning
    // of non-module files by inspecting their header in the mmapped memory.
    // We could also get the UUID from mmapped memory.
    FileFormatAndUUID file_format_uuid =
        GetFileFormatAndUUID(platform, name.GetStringRef());

      LLDB_LOG(log, "Prcheck.");
    if (file_format_uuid.m_triple.empty())
      return {};

    // If a PE module cannot be found locally, we need to download it
    // manually. We cannot use the standard platform downloads because:
    // - lldb-server does not understand build IDs in PEs.
    // - not all PE modules have build IDs (especially the Wine ones don't).
    // Once we equip all PE modules with build IDs and lldb-server understands
    // them, we can remove this download hack.
    if (file_format_uuid.m_triple == "aarch64-pc-windows-msvc") {
      EnsurePEModulePresent(file_spec);
    }

    ModuleSpec new_module_spec(file_spec, file_format_uuid.m_uuid);
    new_module_spec.GetArchitecture().SetTriple(file_format_uuid.m_triple);

    module_sp = target.GetOrCreateModule(new_module_spec, true /* notify */);
      LLDB_LOG(log, "create module {0}.", file_spec.GetPath());
    // If we could not create the module, just skip it.
    if (!module_sp) {
      LLDB_LOG(log, "Could not create module {0}.", file_spec.GetPath());
      return {};
    }

    module_sp->SetPlatformFileSpec(
        FileSpec(name.GetStringRef(), FileSpec::Style::posix));
    // Set the address of the module.
    UpdateLoadedSections(module_sp, 0, address, false);
  }

  return module_sp;
}

void DynamicLoaderPOSIXWineDYLD::LoadModulesFromMaps() {
  Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_DYNAMIC_LOADER));
  Target &target = m_process->GetTarget();
  PlatformSP platform = target.GetPlatform();

  // Read /proc/<pid>/maps from the target.
  std::string command = llvm::formatv("cat /proc/{0}/maps", m_process->GetID());
  int status = -1;
  int signal_no = -1;
  std::string output;
  platform->RunShellCommand(command.c_str(), FileSpec("/"), &status, &signal_no,
                            &output, std::chrono::seconds(15));

  if (status != 0) {
    LLDB_LOG(log, "Failed to invoke '{0}'.", command);
    return;
  }

  // Parse the memory regions, only take those that start with offset zero
  // and their name starts with '/' (to exclude non-file regions).
  std::vector<MemoryRegionInfo> file_regions;
  llvm::StringRef maps_output(output);
  ParseLinuxMapRegions(
      maps_output,
      [&file_regions, &log](llvm::Expected<MemoryRegionInfo> region) -> bool {
        if (region)
          file_regions.push_back(*region);
        else
          LLDB_LOG_ERROR(log, region.takeError(),
                         "Reading memory region from minidump failed: {0}");
        return true;
      });

  ModuleList &modules = m_process->GetTarget().GetImages();

  ConstString candidate_module;
  lldb::addr_t candidate_address = LLDB_INVALID_ADDRESS;
  ModuleList new_modules;

  for (auto &region : file_regions) {
    // Note that dlls often have only the header mapped, but the executable
    // portion of the module is patched, so it does not have a file associated
    // with it. Let us search for a header followed by an executable mapped
    // section either with the same filename or no filename.
    if (region.GetName().GetLength() > 0) {
      if (region.GetFileOffset() == 0) {
        // This looks like a header, let us remember its name and address.
        candidate_module = region.GetName();
        candidate_address = region.GetRange().GetRangeBase();
      } else if (region.GetName() != candidate_module) {
        // If we see a different module name at non-zero offset, invalidate the
        // current candidate.
        candidate_module = ConstString();
        candidate_address = LLDB_INVALID_ADDRESS;
      }
    }

    // If the region is not executable, move on.
    if (region.GetExecutable() != MemoryRegionInfo::eYes)
      continue;

    // If the module name does not look like a file, just skip.
    if (candidate_module.GetLength() == 0 ||
        candidate_module.GetStringRef()[0] != '/')
      continue;
      LLDB_LOG(log, "Pre create not create module {0}.", candidate_module);
    // We have an executable region that follows what looked like a file header.
    // Let us try to load that module.
    ModuleSP module_sp =
        TryLoadModule(modules, candidate_module, candidate_address);
    if (module_sp)
      new_modules.Append(module_sp);

    // We loaded the module (or at least made an honest attempt), so clean
    // the candidate module name so that we don't try again.
    candidate_module = ConstString();
    candidate_address = LLDB_INVALID_ADDRESS;
  }

  m_process->GetTarget().ModulesDidLoad(new_modules);

  // TODO Also handle unloaded modules.

  UpdateBreakpointsForModules();
}

void DynamicLoaderPOSIXWineDYLD::EnsurePEModulePresent(FileSpec &file_spec) {
  Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_DYNAMIC_LOADER));
  Target &target = m_process->GetTarget();
  PlatformSP platform = target.GetPlatform();

  // No need to download anything if the target is not remote.
  if (platform->IsHost())
    return;

  // Get SHA1 of the module from the remote system.
  std::string command = llvm::formatv("sha1sum '{0}'", file_spec.GetPath());
  int status = -1;
  int signal_no = -1;
  std::string output;
  platform->RunShellCommand(command.c_str(), FileSpec(), &status, &signal_no,
                            &output, std::chrono::seconds(15));

  // If checksum fails, skip this module.
  if (status != 0) {
    LLDB_LOG(log, "'{0}' failed with status {1}.", command, status);
    return;
  }

  // Take the hex number prefix of the output.
  llvm::StringRef remote_sha1(output);
  remote_sha1 = remote_sha1.take_while(llvm::isHexDigit);

  // Now try to lookup the file locally.
  FileSpecList search_paths = target.GetExecutableSearchPaths();

  FileSpec cache_spec =
      platform->GetGlobalPlatformProperties().GetModuleCacheDirectory();
  cache_spec.AppendPathComponent(platform->GetName().AsCString());
  cache_spec.AppendPathComponent(".sha1cache");
  cache_spec.AppendPathComponent(remote_sha1);
  search_paths.Append(cache_spec);

  const auto num_directories = search_paths.GetSize();
  for (size_t idx = 0; idx < num_directories; ++idx) {
    auto search_path_spec = search_paths.GetFileSpecAtIndex(idx);
    FileSystem::Instance().Resolve(search_path_spec);
    namespace fs = llvm::sys::fs;
    if (!FileSystem::Instance().IsDirectory(search_path_spec))
      continue;
    search_path_spec.AppendPathComponent(
        file_spec.GetFilename().GetStringRef());
    if (!FileSystem::Instance().Exists(search_path_spec))
      continue;

    // Compute SHA1 of the local candidate. If it is matching, we have our file.
    auto file =
        FileSystem::Instance().Open(search_path_spec, File::eOpenOptionReadOnly);
    if (!file)
      continue;

    llvm::SHA1 hasher;
    std::vector<uint8_t> buffer(0x1000);

    Status status;
    while (true) {
      size_t size = buffer.size();
      status = (*file)->Read(buffer.data(), size);

      if (status.Fail() || size == 0)
        break;

      hasher.update(llvm::ArrayRef<uint8_t>(buffer.data(), size));
    }

    if (status.Fail())
      continue;

    std::string local_sha1 = llvm::toHex(hasher.final(), true);
    if (local_sha1 == remote_sha1) {
      // We found matching file locally, set the file spec and return.
      file_spec = search_path_spec;
      return;
    }
  }

  // We have not found the module in any search path. Let us copy the file
  // to the cache directory.
  // TODO We should lock here to prevent collisions if debugging
  // multiple targets.
  namespace fs = llvm::sys::fs;
  fs::create_directories(cache_spec.GetPath(), true, fs::perms::owner_all);
  cache_spec.AppendPathComponent(file_spec.GetFilename().GetStringRef());
  auto get_file_status = platform->GetFile(file_spec, cache_spec);
  if (get_file_status.Success()) {
    file_spec = cache_spec;
  }
}

lldb::break_id_t
DynamicLoaderPOSIXWineDYLD::SetBreakpoint(const FileSpec &module_spec,
                                          const char *symbol, bool oneShot) {
  FileSpecList module_spec_list;
  module_spec_list.Append(module_spec);

  const bool internal = true;
  const bool hardware = false;
  const LazyBool skip_prologue = eLazyBoolCalculate;
  const lldb::addr_t offset = 0;
  BreakpointSP bp_sp = m_process->GetTarget().CreateBreakpoint(
      &module_spec_list, nullptr, symbol, eFunctionNameTypeAuto,
      eLanguageTypeUnknown, offset, skip_prologue, internal, hardware);

  if (bp_sp->GetNumLocations() == 0) {
    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_DYNAMIC_LOADER));
    LLDB_LOG(log, "SetBreakpoint failed (symbol {0} in module {1}).", symbol,
             module_spec.GetPath());
  }

  bp_sp->SetOneShot(oneShot);
  bp_sp->SetCallback(BreakpointHandlerCallback, this);
  bp_sp->SetBreakpointKind("wine-integration-event");

  return bp_sp->GetID();
}

lldb::break_id_t DynamicLoaderPOSIXWineDYLD::SetReturnBreakpoint(
    StoppointCallbackContext *context) {
  ThreadSP thread = context->exe_ctx_ref.GetThreadSP();
  if (!thread)
    return LLDB_INVALID_BREAK_ID;
  uint32_t frame_index = 0;
  StackFrameSP frame_sp = thread->GetStackFrameAtIndex(frame_index);

  // Find the real parent frame to return to, but exclude inlining parents. That
  // is somewhat dangerous, but much simpler than trying to stop after the
  // inlined callsite.
  while (frame_sp && frame_sp->IsInlined()) {
    frame_index++;
    frame_sp = thread->GetStackFrameAtIndex(frame_index);
  }
  if (!frame_sp)
    return LLDB_INVALID_BREAK_ID;
  StackFrameSP return_frame_sp(thread->GetStackFrameAtIndex(frame_index + 1));
  if (!return_frame_sp)
    return LLDB_INVALID_BREAK_ID;
  lldb::addr_t return_address =
      return_frame_sp->GetFrameCodeAddress().GetLoadAddress(
          &m_process->GetTarget());

  const bool internal = true;
  const bool hardware = false;
  BreakpointSP bp_sp = m_process->GetTarget().CreateBreakpoint(
      return_address, internal, hardware);

  if (bp_sp->GetNumLocations() == 0) {
    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_DYNAMIC_LOADER));
    LLDB_LOG(log, "Could not place return breakpoint at address {0:x}.",
             return_address);
  }

  bp_sp->SetOneShot(true);
  bp_sp->SetCallback(BreakpointHandlerCallback, this);
  bp_sp->SetBreakpointKind("wine-integration-event");

  return bp_sp->GetID();
}

// static
bool DynamicLoaderPOSIXWineDYLD::BreakpointHandlerCallback(
    void *baton, StoppointCallbackContext *context, lldb::user_id_t break_id,
    lldb::user_id_t break_loc_id) {
  break_id_t converted_break_id = static_cast<break_id_t>(break_id);
  static_cast<DynamicLoaderPOSIXWineDYLD *>(baton)->BreakpointHandler(
      context, converted_break_id);
  return false;
}

void DynamicLoaderPOSIXWineDYLD::BreakpointHandler(
    StoppointCallbackContext *context, lldb::break_id_t break_id) {
  if (break_id == m_break_preloader_wld_start) {
    m_break_preloader_wld_start = LLDB_INVALID_BREAK_ID;
    m_break_preloader_wld_start_finish = SetReturnBreakpoint(context);
  } else if (break_id == m_break_preloader_wld_start_finish) {
    m_break_preloader_wld_start_finish = LLDB_INVALID_BREAK_ID;
    LoadModulesFromMaps();
  } else if (break_id == m_break_ntdll_map_image) {
    // Let us clear the previously set finish breakpoint if it was not hit.
    if (m_break_ntdll_map_image_finish != LLDB_INVALID_BREAK_ID) {
      m_process->GetTarget().RemoveBreakpointByID(
          m_break_ntdll_map_image_finish);
    }
    m_break_ntdll_map_image_finish = SetReturnBreakpoint(context);
  } else if (break_id == m_break_ntdll_map_image_finish) {
    m_process->GetTarget().RemoveBreakpointByID(m_break_ntdll_map_image_finish);
    m_break_ntdll_map_image_finish = LLDB_INVALID_BREAK_ID;
    LoadModulesFromMaps();
  } else if (break_id == m_break_ld_dl_debug_state) {
    LoadModulesFromMaps();
  }
}

void DynamicLoaderPOSIXWineDYLD::UpdateBreakpointsForModules() {
  // If we already have the dynamic linker breakpoints, then just return.
  if (m_break_ntdll_map_image != LLDB_INVALID_BREAK_ID &&
      m_break_ld_dl_debug_state != LLDB_INVALID_BREAK_ID) {
    return;
  }

  // Otherwise go through the list of modules and try to find the linkers.
  ModuleList &modules = m_process->GetTarget().GetImages();

  FileSpec ntdll_module;
  FileSpec ld_module;

  {
    std::lock_guard<std::recursive_mutex> guard(modules.GetMutex());
    for (size_t i = 0; i < modules.GetSize(); i++) {
      auto module_sp = modules.GetModuleAtIndex(i);
      auto filename = module_sp->GetFileSpec().GetFilename().GetStringRef();
      if (filename.startswith(g_ld_filename_prefix)) {
        // TODO validate this better, perhaps by looking up
        // __dl_debug_state in the module.
        ld_module = module_sp->GetFileSpec();
      }
      if (filename == g_ntdll_filename) {
        ntdll_module = module_sp->GetFileSpec();
      }
    }
  }

  if (m_break_ld_dl_debug_state == LLDB_INVALID_BREAK_ID) {
    if (ld_module) {
      // TODO Instead of setting the breakpoint to the specific
      // symbol name, we should harvest the breakpoint address from the wine64's
      // .dynamic/DT_DEBUG structure. That way, we wouldn't depend on
      // the exact name of the linux loader and on the presence of debug
      // info/symbols.
      m_break_ld_dl_debug_state =
          SetBreakpoint(ld_module, g_ld_load_symbol, false);
    } else if (m_break_preloader_wld_start == LLDB_INVALID_BREAK_ID) {
      // If is not loaded yet, let us get through the preloader startup
      // code that loads ld.
      m_break_preloader_wld_start =
          SetBreakpoint(modules.GetModuleAtIndex(0)->GetFileSpec(),
                        g_wine_preloader_load_symbol, true);
    }
  }

  if (m_break_ntdll_map_image == LLDB_INVALID_BREAK_ID && ntdll_module) {
    m_break_ntdll_map_image =
        SetBreakpoint(ntdll_module, g_ntdll_load_symbol, false);
  }
}
