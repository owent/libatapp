# =========== 3rd_party ===========
if (NOT PROJECT_3RD_PARTY_PACKAGE_DIR)
    set (PROJECT_3RD_PARTY_PACKAGE_DIR "${CMAKE_CURRENT_LIST_DIR}/packages")
endif ()
if (NOT PROJECT_3RD_PARTY_INSTALL_DIR)
    set (PROJECT_3RD_PARTY_INSTALL_DIR "${CMAKE_CURRENT_LIST_DIR}/install/${PROJECT_PREBUILT_PLATFORM_NAME}")
endif ()

if (NOT EXISTS ${PROJECT_3RD_PARTY_PACKAGE_DIR})
    file(MAKE_DIRECTORY ${PROJECT_3RD_PARTY_PACKAGE_DIR})
endif ()

if (NOT EXISTS ${PROJECT_3RD_PARTY_INSTALL_DIR})
    file(MAKE_DIRECTORY ${PROJECT_3RD_PARTY_INSTALL_DIR})
endif ()

set (PROJECT_3RD_PARTY_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})

include("${PROJECT_3RD_PARTY_ROOT_DIR}/compression/import.cmake")

if (LIBATBUS_ROOT AND EXISTS "${LIBATBUS_ROOT}/3rd_party/protobuf/protobuf.cmake")
    include("${LIBATBUS_ROOT}/3rd_party/protobuf/protobuf.cmake")
endif ()

include("${PROJECT_3RD_PARTY_ROOT_DIR}/yaml-cpp/yaml-cpp.cmake")

# =========== 3rd_party - rapidjson ===========
include("${PROJECT_3RD_PARTY_ROOT_DIR}/rapidjson/rapidjson.cmake")

# =========== 3rd_party - crypto ===========
include("${PROJECT_3RD_PARTY_ROOT_DIR}/ssl/port.cmake")

# =========== 3rd_party - libcurl ===========
include("${PROJECT_3RD_PARTY_ROOT_DIR}/libcurl/libcurl.cmake")

# =========== 3rd_party - grpc ===========
# Must be imported after ssl,protobuf,zlib
include("${PROJECT_3RD_PARTY_ROOT_DIR}/grpc/import.cmake")

include("${PROJECT_3RD_PARTY_ROOT_DIR}/atframe_utils/libatframe_utils.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/libatbus/libatbus.cmake")

if (NOT 3RD_PARTY_PROTOBUF_BIN_PROTOC OR (NOT 3RD_PARTY_PROTOBUF_LINK_NAME AND NOT 3RD_PARTY_PROTOBUF_INC_DIR))
    include("${LIBATBUS_ROOT}/3rd_party/protobuf/protobuf.cmake")
endif ()
