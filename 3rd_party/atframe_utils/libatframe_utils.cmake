﻿if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.10")
  include_guard(GLOBAL)
endif()

if(TARGET ${3RD_PARTY_ATFRAME_UTILS_LINK_NAME})
  message(
    STATUS
      "${3RD_PARTY_ATFRAME_UTILS_LINK_NAME} already exist, use it for ${PROJECT_NAME} directly.")
else()
  if(NOT EXISTS "${CMAKE_BINARY_DIR}/deps/${3RD_PARTY_ATFRAME_UTILS_LINK_NAME}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/deps/${3RD_PARTY_ATFRAME_UTILS_LINK_NAME}")
  endif()
  add_subdirectory(${3RD_PARTY_ATFRAME_UTILS_PKG_DIR}
                   "${CMAKE_BINARY_DIR}/deps/${3RD_PARTY_ATFRAME_UTILS_LINK_NAME}")
endif()

# =========== 3rdparty atframe_utils ==================
list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES ${3RD_PARTY_ATFRAME_UTILS_LINK_NAME})
