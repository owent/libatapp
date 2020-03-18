cmake_minimum_required(VERSION 3.7.0)
cmake_policy(SET CMP0054 NEW)
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.13")
    cmake_policy(SET CMP0074 NEW)
endif()

project(libatapp)

set(LIBATAPP_VERSION_MAJOR "0")
set(LIBATAPP_VERSION_MINOR "5")
set(LIBATAPP_VERSION_PATCH "1")
set(LIBATAPP_VERSION "${LIBATAPP_VERSION_MAJOR}.${LIBATAPP_VERSION_MINOR}.${LIBATAPP_VERSION_PATCH}")

# 准备下载依赖组件
include ("${CMAKE_CURRENT_LIST_DIR}/3rd_party/atframe_utils/libatframe_utils.prepare.cmake")

########################################################################
# CMake 模块 (递归包含模块, 带颜色输出模块, 平台检测模块)
set (PROJECT_CMAKE_MODULE_DIR "${3RD_PARTY_ATFRAME_UTILS_PKG_DIR}/project/cmake")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_CMAKE_MODULE_DIR}/modules")


include("${PROJECT_CMAKE_MODULE_DIR}/modules/IncludeDirectoryRecurse.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/modules/EchoWithColor.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/modules/FindConfigurePackage.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/project/cmake/ProjectBuildOption.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/FindPlatform.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/ProjectTools.cmake")


#####################################################################
# 导入编译器和编译选项配置
include("${PROJECT_CMAKE_MODULE_DIR}/CompilerOption.cmake")
include("${PROJECT_CMAKE_MODULE_DIR}/TargetOption.cmake")
EchoWithColor(COLOR GREEN "-- Build Type: ${CMAKE_BUILD_TYPE}")

unset(PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS)
unset(PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS)
unset(PROJECT_LIBATAPP_PUBLIC_LINK_NAMES)
unset(PROJECT_LIBATAPP_INTERFACE_LINK_NAMES)
unset(PROJECT_LIBATAPP_EXPORT_TARGETS)
########################################################################
# 导入项目配置
## 导入所有 macro 定义
include("${CMAKE_CURRENT_LIST_DIR}/3rd_party/3rd_party.macro.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/include/include.macro.cmake")

if (COMPILER_STRICT_EXTRA_CFLAGS)
    add_definitions(${COMPILER_STRICT_EXTRA_CFLAGS})
endif ()

if (COMPILER_STRICT_CFLAGS)
    add_definitions(${COMPILER_STRICT_CFLAGS})
endif ()

include(GNUInstallDirs)

set (LIBATAPP_MACRO_BUSID_TYPE ${ATBUS_MACRO_BUSID_TYPE})
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/include/atframe/atapp_config.h.in"
    "${CMAKE_CURRENT_LIST_DIR}/include/atframe/atapp_config.h"
    @ONLY
)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/src")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/binding/c")

if (PROJECT_ENABLE_SAMPLE)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/sample")
endif()

# if (PROJECT_ENABLE_UNITTEST)
#     include ("${3RD_PARTY_ATFRAME_UTILS_PKG_DIR}/test/test.utils-macro.cmake")
#     add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/test")
# endif()

if (PROJECT_ENABLE_TOOLS)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/tools")
endif()

## 生成文档和导入配置

# Install configuration
set(CMAKE_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake" CACHE STRING "Directory relative to CMAKE_INSTALL to install the cmake configuration files")

include(CMakePackageConfigHelpers)

file(MAKE_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake")

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/libatapp-config.cmake.in"
    "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/libatapp-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
    PATH_VARS LIBATAPP_VERSION CMAKE_INSTALL_LIBDIR PROJECT_SOURCE_DIR
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

write_basic_package_version_file(
    "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/libatapp-config-version.cmake"
    VERSION ${LIBATAPP_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/libatapp-config.cmake" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/libatapp-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_CMAKEDIR}
)

export(TARGETS ${PROJECT_LIBATAPP_EXPORT_TARGETS}
    FILE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/cmake/${PROJECT_LIBATAPP_EXPORT_NAME}.cmake"
    NAMESPACE atframework::
    # EXPORT_LINK_INTERFACE_LIBRARIES
)

install(
    EXPORT ${PROJECT_LIBATAPP_EXPORT_NAME}
    NAMESPACE "atframework::"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake"
)
