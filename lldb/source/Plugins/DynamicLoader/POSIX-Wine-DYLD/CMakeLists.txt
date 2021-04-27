lldb_tablegen(DynamicLoaderPOSIXWineDYLDProperties.inc -gen-lldb-property-defs
  SOURCE DynamicLoaderPOSIXWineDYLDProperties.td
  TARGET LLDBPluginDynamicLoaderPOSIXWineDYLDPropertiesGen)

lldb_tablegen(DynamicLoaderPOSIXWineDYLDPropertiesEnum.inc -gen-lldb-property-enum-defs
  SOURCE DynamicLoaderPOSIXWineDYLDProperties.td
  TARGET LLDBPluginDynamicLoaderPOSIXWineDYLDPropertiesEnumGen)

add_lldb_library(lldbPluginDynamicLoaderPosixWineDYLD PLUGIN
  DynamicLoaderPOSIXWineDYLD.cpp

  LINK_LIBS
    lldbCore
    lldbPluginDynamicLoaderPosixDYLD
    lldbPluginProcessUtility
    lldbTarget
  LINK_COMPONENTS
    Support
  )

add_dependencies(lldbPluginDynamicLoaderPosixWineDYLD
  LLDBPluginDynamicLoaderPOSIXWineDYLDPropertiesGen
  LLDBPluginDynamicLoaderPOSIXWineDYLDPropertiesEnumGen)