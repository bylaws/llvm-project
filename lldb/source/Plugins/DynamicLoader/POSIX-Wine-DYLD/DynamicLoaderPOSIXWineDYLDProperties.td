include "../../../../include/lldb/Core/PropertiesBase.td"

let Definition = "posixwinedyld" in {
  def RemoteObjdumpPath: Property<"remote-objdump-path", "String">,
    Global,
    DefaultStringValue<"llvm-objdump">,
    Desc<"The path to the llvm-objdump binary.">;
  def UseWineDynamicLoader: Property<"use-wine-dynamic-loader", "Boolean">,
    Global,
    DefaultTrue,
    Desc<"Enable the Wine dynamic loader plugin.">;
}