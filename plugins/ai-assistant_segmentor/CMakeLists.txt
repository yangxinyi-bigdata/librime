project(rime-ai-assistant_segmentor)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src ai_assistant_segmentor_src)

add_library(rime-ai-assistant_segmentor-objs OBJECT ${ai_assistant_segmentor_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-ai-assistant_segmentor-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-ai-assistant_segmentor PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-ai-assistant_segmentor-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "ai_assistant_segmentor" PARENT_SCOPE)
