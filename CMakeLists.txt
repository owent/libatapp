cmake_minimum_required(VERSION 3.24.0)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.27.0")
  cmake_policy(SET CMP0144 NEW)
endif()

enable_testing()

project(
  libatapp
  VERSION "0.14.0"
  HOMEPAGE_URL "https://github.com/atframework/libatapp"
  LANGUAGES C CXX)

if(NOT DEFINED __COMPILER_OPTION_LOADED)
  if(MSVC)
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/GR-?" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    string(REGEX REPLACE "-f(no-)?exceptions" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "-f(no-)?rtti" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  endif()
endif()

set(LIBATAPP_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(LIBATAPP_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(LIBATAPP_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(LIBATAPP_VERSION "${PROJECT_VERSION}")

include("${CMAKE_CURRENT_LIST_DIR}/project/cmake/ProjectBuildOption.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/third_party/Repository.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atframework/Repository.cmake")

unset(PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS)
unset(PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS)
unset(PROJECT_LIBATAPP_PUBLIC_LINK_NAMES)

# ######################################################################################################################
include("${CMAKE_CURRENT_LIST_DIR}/include/include.macro.cmake")

if(COMPILER_STRICT_EXTRA_CFLAGS)
  list(APPEND PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS ${COMPILER_STRICT_EXTRA_CFLAGS})
endif()

if(COMPILER_STRICT_CFLAGS)
  list(APPEND PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS ${COMPILER_STRICT_CFLAGS})
endif()

if(COMPILER_STRICT_RECOMMEND_EXTRA_CFLAGS)
  list(APPEND PROJECT_LIBATBUS_PRIVATE_COMPILE_OPTIONS ${COMPILER_STRICT_RECOMMEND_EXTRA_CFLAGS})
endif()

include(GNUInstallDirs)

set(LIBATAPP_MACRO_BUSID_TYPE ${ATBUS_MACRO_BUSID_TYPE})
if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "5.0.0")
    set(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST YES)
  else() # Detect if using legacy ABI
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles(
      "
#include <stdint.h>
#include <cstddef>
#include <list>

int main() {
  static_assert(sizeof(std::list<size_t>) <= 2 * sizeof(size_t), \"Legacy ABI\");
  return 0;
}
    "
      LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST)
  endif()
endif()
set(PROJECT_LIBATAPP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/_generated")
file(MAKE_DIRECTORY "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe")
file(MAKE_DIRECTORY "${PROJECT_LIBATAPP_GENERATED_DIR}/src")
file(MAKE_DIRECTORY "${PROJECT_LIBATAPP_GENERATED_DIR}/temp")
configure_file("${CMAKE_CURRENT_LIST_DIR}/include/atframe/atapp_config.h.in"
               "${PROJECT_LIBATAPP_GENERATED_DIR}/temp/atapp_config.h" @ONLY)
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_LIBATAPP_GENERATED_DIR}/temp/atapp_config.h"
                        "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/src")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/binding/c")

if(PROJECT_ENABLE_SAMPLE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/sample")
endif()

if(PROJECT_ENABLE_UNITTEST OR BUILD_TESTING)
  include("${ATFRAMEWORK_ATFRAME_UTILS_REPO_DIR}/test/test.utils-macro.cmake")
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/test")
endif()

if(PROJECT_ENABLE_TOOLS)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/tools")
endif()

# Install configuration
set(CMAKE_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    CACHE STRING "Directory relative to CMAKE_INSTALL to install the cmake configuration files")

include(CMakePackageConfigHelpers)

file(MAKE_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_NAME}")

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/libatapp-config.cmake.in"
  "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_NAME}/${PROJECT_NAME}-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
  PATH_VARS LIBATAPP_VERSION CMAKE_INSTALL_LIBDIR PROJECT_SOURCE_DIR
  NO_CHECK_REQUIRED_COMPONENTS_MACRO)

write_basic_package_version_file(
  "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_NAME}/${PROJECT_NAME}-config-version.cmake"
  VERSION ${LIBATAPP_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_NAME}/${PROJECT_NAME}-config.cmake"
              "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_NAME}/${PROJECT_NAME}-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_CMAKEDIR})

export(
  EXPORT ${PROJECT_LIBATAPP_EXPORT_NAME}
  NAMESPACE atframework::
  FILE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_NAME}/${PROJECT_LIBATAPP_EXPORT_NAME}.cmake")

install(
  EXPORT ${PROJECT_LIBATAPP_EXPORT_NAME}
  NAMESPACE "atframework::"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")
