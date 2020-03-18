if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.10")
    include_guard(GLOBAL)
endif()

if (NOT TARGET ${3RD_PARTY_ATFRAME_UTILS_LINK_NAME})
    add_subdirectory(${3RD_PARTY_ATFRAME_UTILS_PKG_DIR})
endif ()

# =========== 3rdparty atframe_utils ==================
list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES ${3RD_PARTY_ATFRAME_UTILS_LINK_NAME})

