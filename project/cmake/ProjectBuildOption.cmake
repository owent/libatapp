# 默认配置选项
#####################################################################

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

option(PROJECT_RESET_DENPEND_REPOSITORIES "Reset depended repositories if it's already exists." OFF)
option(PROJECT_FIND_CONFIGURE_PACKAGE_PARALLEL_BUILD
       "Parallel building for FindConfigurePackage. It's usually useful for some CI with low memory." ON)
option(ATFRAMEWORK_USE_DYNAMIC_LIBRARY "Build and linking with dynamic libraries." OFF)
