project(rime-keylogger)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src keylogger_src)

add_library(rime-keylogger-objs OBJECT ${keylogger_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-keylogger-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-keylogger PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-keylogger-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "keylogger" PARENT_SCOPE)
