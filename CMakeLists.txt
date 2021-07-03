cmake_minimum_required(VERSION 3.16.0)
cmake_policy(SET CMP0054 NEW)
cmake_policy(SET CMP0022 NEW)
cmake_policy(SET CMP0067 NEW)
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0091 NEW)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.19.0")
  cmake_policy(SET CMP0111 NEW)
endif()

enable_testing()

project(
  libatapp
  VERSION "0.10.0"
  HOMEPAGE_URL "https://github.com/atframework/libatapp"
  LANGUAGES C CXX)

set(LIBATAPP_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(LIBATAPP_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(LIBATAPP_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(LIBATAPP_VERSION "${PROJECT_VERSION}")

include("${CMAKE_CURRENT_LIST_DIR}/project/cmake/ProjectBuildOption.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/third_party/Repository.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atframework/Repository.cmake")

echowithcolor(COLOR GREEN "-- Build Type: ${CMAKE_BUILD_TYPE}")

unset(PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS)
unset(PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS)
unset(PROJECT_LIBATAPP_PUBLIC_LINK_NAMES)

# ######################################################################################################################
# 导入项目配置 导入所有 macro 定义
include("${CMAKE_CURRENT_LIST_DIR}/include/include.macro.cmake")

if(COMPILER_STRICT_EXTRA_CFLAGS)
  list(APPEND PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS ${COMPILER_STRICT_EXTRA_CFLAGS})
endif()

if(COMPILER_STRICT_CFLAGS)
  list(APPEND PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS ${COMPILER_STRICT_CFLAGS})
endif()

include(GNUInstallDirs)

set(LIBATAPP_MACRO_BUSID_TYPE ${ATBUS_MACRO_BUSID_TYPE})
if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
  # TODO 不应该仅按版本号判定，需要检测一下编译选项(std::list<T>的大小？)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "5.0.0")
    set(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST YES)
  endif()
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/include/atframe/atapp_config.h.in"
               "${CMAKE_CURRENT_LIST_DIR}/include/atframe/atapp_config.h" @ONLY)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/src")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/binding/c")

if(PROJECT_ENABLE_SAMPLE)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/sample")
endif()

if(PROJECT_ENABLE_UNITTEST)
  include("${ATFRAMEWORK_ATFRAME_UTILS_REPO_DIR}/test/test.utils-macro.cmake")
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/test")
endif()

if(PROJECT_ENABLE_TOOLS)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/tools")
endif()

# 生成文档和导入配置

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
