project(rime-aipara)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src aipara_src)

add_library(rime-aipara-objs OBJECT ${aipara_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-aipara-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-aipara PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-aipara-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "aipara" PARENT_SCOPE)
