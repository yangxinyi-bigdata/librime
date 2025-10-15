aux_source_directory(. rime_test_src)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/test)
add_executable(rime_test ${rime_test_src})

set(rime_test_gtest_targets "")
if(TARGET GTest::gtest)
  list(APPEND rime_test_gtest_targets GTest::gtest)
elseif(DEFINED GTEST_LIBRARIES AND NOT "${GTEST_LIBRARIES}" STREQUAL "")
  list(APPEND rime_test_gtest_targets ${GTEST_LIBRARIES})
elseif(DEFINED GTest_LIBRARIES AND NOT "${GTest_LIBRARIES}" STREQUAL "")
  list(APPEND rime_test_gtest_targets ${GTest_LIBRARIES})
else()
  message(FATAL_ERROR "GTest libraries not found; please make sure GTest is available.")
endif()

target_link_libraries(rime_test
  ${rime_library}
  ${rime_dict_library}
  ${rime_gears_library}
  ${rime_test_gtest_targets})
target_include_directories(rime_test
  BEFORE PRIVATE
  ${PROJECT_SOURCE_DIR}/include)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(rime_test PRIVATE RIME_IMPORTS)
endif(BUILD_SHARED_LIBS)

file(GLOB test_data_files ${PROJECT_SOURCE_DIR}/data/test/*.yaml)
file(COPY ${test_data_files} DESTINATION ${EXECUTABLE_OUTPUT_PATH})

add_test(NAME rime_test
  COMMAND rime_test
  WORKING_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
