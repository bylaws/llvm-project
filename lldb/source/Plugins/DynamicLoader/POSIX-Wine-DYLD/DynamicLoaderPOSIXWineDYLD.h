//===-- DynamicLoaderPOSIXWineDYLD.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_DYNAMICLOADER_POSIX_WINE_DYLD_DYNAMICLOADERPOSIXWINEDYLD_H
#define LLDB_SOURCE_PLUGINS_DYNAMICLOADER_POSIX_WINE_DYLD_DYNAMICLOADERPOSIXWINEDYLD_H

#include "lldb/Target/DynamicLoader.h"
#include "lldb/lldb-forward.h"

#include "Plugins/DynamicLoader/POSIX-DYLD/DynamicLoaderPOSIXDYLD.h"

#include <map>

namespace lldb_private {

class DynamicLoaderPOSIXWineDYLD : public DynamicLoaderPOSIXDYLD {
public:
  DynamicLoaderPOSIXWineDYLD(Process *process);

  ~DynamicLoaderPOSIXWineDYLD() override;

  static void Initialize();
  static void Terminate();
  static llvm::StringRef GetPluginNameStatic();
  static llvm::StringRef GetPluginDescriptionStatic();

  static DynamicLoader *CreateInstance(Process *process, bool force);
  static void DebuggerInitialize(Debugger &debugger);

  llvm::StringRef GetPluginName() override;

  void DidAttach() override;

private:
  // Adds modules from /proc/<pid>/maps, also places breakpoints in both
  // Wine and Linux dynamic linkers so that the list of modules gets reloaded
  // automatically.
  void LoadModulesFromMaps();
  lldb::ModuleSP TryLoadModule(ModuleList &modules, ConstString name,
                               lldb::addr_t address);

  // Downloads a PE module to the workstation if necessary.
  void EnsurePEModulePresent(FileSpec &file_spec);

  // Tries to place breakpoints that trigger when a new module is loaded.
  // The breakpoints are only created if the dynamic linker modules are present.
  void UpdateBreakpointsForModules();
  lldb::break_id_t SetBreakpoint(const FileSpec &module_spec,
                                 const char *symbol, bool oneShot);
  lldb::break_id_t SetReturnBreakpoint(StoppointCallbackContext *context);
  static bool BreakpointHandlerCallback(void *baton,
                                        StoppointCallbackContext *context,
                                        lldb::user_id_t break_id,
                                        lldb::user_id_t break_loc_id);
  void BreakpointHandler(StoppointCallbackContext *context,
                         lldb::break_id_t break_id);

  lldb::break_id_t m_break_preloader_wld_start = LLDB_INVALID_BREAK_ID;
  lldb::break_id_t m_break_preloader_wld_start_finish = LLDB_INVALID_BREAK_ID;
  lldb::break_id_t m_break_ntdll_map_image = LLDB_INVALID_BREAK_ID;
  lldb::break_id_t m_break_ntdll_map_image_finish = LLDB_INVALID_BREAK_ID;
  lldb::break_id_t m_break_ld_dl_debug_state = LLDB_INVALID_BREAK_ID;
};

} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_DYNAMICLOADER_POSIX_WINE_DYLD_DYNAMICLOADERPOSIXWINEDYLD_H
