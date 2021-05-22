echowithcolor(COLOR GREEN "-- Configure Unit Test ${CMAKE_CURRENT_LIST_DIR}")

include("${PROJECT_TEST_BAS_DIR}/test.build_bin.cmake")

file(
  GLOB_RECURSE
  PROJECT_TEST_SRC_LIST
  ${PROJECT_TEST_SRC_DIR}/app/*.cpp
  ${PROJECT_TEST_SRC_DIR}/frame/*.h
  ${PROJECT_TEST_SRC_DIR}/frame/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/*.hpp
  ${CMAKE_CURRENT_LIST_DIR}/*.h
  ${CMAKE_CURRENT_LIST_DIR}/*.c
  ${CMAKE_CURRENT_LIST_DIR}/*.cpp
  ${CMAKE_CURRENT_LIST_DIR}/*.cc
  ${CMAKE_CURRENT_LIST_DIR}/*.cxx)
source_group_by_dir(PROJECT_TEST_SRC_LIST)

# ============ test - coroutine test frame ============
set(CMAKE_BUILD_RPATH "$ORIGIN/../lib" "$ORIGIN/../lib64")
if(NOT (WIN32 AND BUILD_SHARED_LIBS))
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test")
endif()

atframe_add_test_executable(atapp_unit_test ${PROJECT_TEST_SRC_LIST})
target_compile_options(atapp_unit_test PRIVATE ${PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS})
target_link_libraries(atapp_unit_test atapp)

add_test(test atapp_unit_test)
