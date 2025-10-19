project(rime-aipara)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src aipara_src)
aux_source_directory(src/common aipara_common_src)
set(aipara_src ${aipara_src} ${aipara_common_src})

add_library(rime-aipara-objs OBJECT ${aipara_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-aipara-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()
target_include_directories(rime-aipara-objs PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/opencc/deps/rapidjson-1.1.0
  ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_library(ZeroMQ_LIBRARY NAMES zmq)
if(NOT ZeroMQ_LIBRARY)
  message(FATAL_ERROR "libzmq 未找到，请安装 ZeroMQ。")
endif()

set(plugin_name rime-aipara PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-aipara-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} ${ZeroMQ_LIBRARY} PARENT_SCOPE)
set(plugin_modules "aipara" PARENT_SCOPE)
