# =========== 3rd_party ===========
set (PROJECT_3RD_PARTY_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR})

if (LIBATBUS_ROOT AND EXISTS "${LIBATBUS_ROOT}/3rd_party/protobuf/protobuf.cmake")
    include("${LIBATBUS_ROOT}/3rd_party/protobuf/protobuf.cmake")
endif ()

include("${PROJECT_3RD_PARTY_ROOT_DIR}/yaml-cpp/yaml-cpp.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/atframe_utils/libatframe_utils.cmake")
include("${PROJECT_3RD_PARTY_ROOT_DIR}/libatbus/libatbus.cmake")


if (NOT 3RD_PARTY_PROTOBUF_BIN_PROTOC OR (NOT 3RD_PARTY_PROTOBUF_LINK_NAME AND NOT 3RD_PARTY_PROTOBUF_INC_DIR))
    include("${LIBATBUS_ROOT}/3rd_party/protobuf/protobuf.cmake")
endif ()
