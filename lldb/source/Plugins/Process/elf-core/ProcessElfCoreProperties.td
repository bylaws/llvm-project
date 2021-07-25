include "../../../../include/lldb/Core/PropertiesBase.td"

let Definition = "elfcore" in {
  def LoadModulesFromCoreNote: Property<"load-modules-from-core-note", "Boolean">,
    Global,
    DefaultTrue,
    Desc<"Load modules from the core file's NtFile note.">;
}