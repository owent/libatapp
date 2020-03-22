# 默认配置选项
#####################################################################

# libuv选项
set(LIBUV_ROOT "" CACHE STRING "libuv root directory")

# 测试配置选项
set(GTEST_ROOT "" CACHE STRING "GTest root directory")
set(BOOST_ROOT "" CACHE STRING "Boost root directory")
option(PROJECT_TEST_ENABLE_BOOST_UNIT_TEST "Enable boost unit test." OFF)

option(PROJECT_RESET_DENPEND_REPOSITORIES "Reset depended repositories if it's already exists." OFF)
option(PROJECT_GIT_CLONE_REMOTE_ORIGIN_DISABLE_SSH "Do not try to use ssh url when clone dependency." OFF)
