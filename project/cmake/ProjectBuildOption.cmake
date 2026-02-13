# Copyright 2026 atframework
# 默认配置选项
# ##################################################################################################

include("${CMAKE_CURRENT_LIST_DIR}/FetchDependeny.cmake")
include(IncludeDirectoryRecurse)
include(EchoWithColor)

# libuv选项
set(LIBUV_ROOT
    ""
    CACHE STRING "libuv root directory")

# 测试配置选项
set(GTEST_ROOT
    ""
    CACHE STRING "GTest root directory")
set(BOOST_ROOT
    ""
    CACHE STRING "Boost root directory")
option(PROJECT_TEST_ENABLE_BOOST_UNIT_TEST "Enable boost unit test." OFF)

option(ATFRAMEWORK_USE_DYNAMIC_LIBRARY "Build and linking with dynamic libraries." OFF)

set(LIBATAPP_MACRO_HASH_MAGIC_NUMBER
    "0x01000193U"
    CACHE STRING "Magic number of libatapp")

math(EXPR ATFRAMEWORK_ATAPP_ABI_NUMBER "${LIBATAPP_VERSION_MAJOR}*1000+${LIBATAPP_VERSION_MINOR}")
set(ATFRAMEWORK_ATAPP_ABI_TAG
    "v${ATFRAMEWORK_ATAPP_ABI_NUMBER}"
    CACHE STRING "libatapp ABI tag")

