echowithcolor(COLOR GREEN "-- Configure ${CMAKE_CURRENT_LIST_DIR}")

set(PROJECT_LIBATAPP_ROOT_SRC_DIR ${CMAKE_CURRENT_LIST_DIR})

list(APPEND PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS ${PROJECT_LIBATAPP_ROOT_INC_DIR})

set(PROJECT_LIBATAPP_SRC_LIST
    "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe/atapp_config.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp_conf.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp_conf_rapidjson.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp_log_sink_maker.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp_module_impl.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/connectors/atapp_connector_atbus.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/connectors/atapp_connector_impl.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/connectors/atapp_connector_loopback.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/connectors/atapp_endpoint.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/etcdcli/etcd_cluster.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/etcdcli/etcd_def.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/etcdcli/etcd_discovery.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/etcdcli/etcd_keepalive.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/etcdcli/etcd_packer.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/etcdcli/etcd_watcher.h"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/modules/etcd_module.h"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/atapp.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/atapp_conf.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/atapp_conf_rapidjson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/atapp_log_sink_maker.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/atapp_module_impl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/connectors/atapp_connector_atbus.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/connectors/atapp_connector_impl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/connectors/atapp_connector_loopback.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/connectors/atapp_endpoint.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/etcdcli/etcd_cluster.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/etcdcli/etcd_discovery.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/etcdcli/etcd_keepalive.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/etcdcli/etcd_packer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/etcdcli/etcd_watcher.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/atframe/modules/etcd_module.cpp")
source_group_by_dir(PROJECT_LIBATAPP_SRC_LIST)

set(PROJECT_LIBATAPP_PROTOCOL_SRC_LIST "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe/atapp_conf.pb.h"
                                       "${PROJECT_LIBATAPP_GENERATED_DIR}/src/atapp_conf.pb.cc")

# ============ libatapp - src ============
add_custom_command(
  OUTPUT ${PROJECT_LIBATAPP_PROTOCOL_SRC_LIST}
  COMMAND
    "${ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_BIN_PROTOC}" "-I" "${PROJECT_LIBATAPP_ROOT_INC_DIR}" "-I"
    "${PROJECT_THIRD_PARTY_INSTALL_DIR}/include" "-I" "${ATFRAMEWORK_LIBATBUS_REPO_DIR}/include"
    "--cpp_out=dllexport_decl=LIBATAPP_MACRO_PROTOCOL_API:${PROJECT_LIBATAPP_GENERATED_DIR}/temp"
    "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp_conf.proto"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_LIBATAPP_GENERATED_DIR}/temp/atframe/atapp_conf.pb.cc"
          "${PROJECT_LIBATAPP_GENERATED_DIR}/src/atapp_conf.pb.cc"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_LIBATAPP_GENERATED_DIR}/temp/atframe/atapp_conf.pb.h"
          "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe/atapp_conf.pb.h"
  DEPENDS "${PROJECT_LIBATAPP_ROOT_INC_DIR}/atframe/atapp_conf.proto"
          "${ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_BIN_PROTOC}"
  WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
  COMMENT
    "Generate ${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe/atapp_conf.pb.h and ${PROJECT_LIBATAPP_GENERATED_DIR}/src/atapp_conf.pb.cc"
)

if(NOT DEFINED LIBATAPP_MACRO_PROTOCOL_USE_DYNAMIC_LIBRARY)
  if(BUILD_SHARED_LIBS OR ATFRAMEWORK_USE_DYNAMIC_LIBRARY)
    if(DEFINED ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_ALLOW_SHARED_LIBS)
      set(LIBATAPP_MACRO_PROTOCOL_USE_DYNAMIC_LIBRARY
          ${ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_ALLOW_SHARED_LIBS})
    else()
      set(LIBATAPP_MACRO_PROTOCOL_USE_DYNAMIC_LIBRARY TRUE)
    endif()
  else()
    set(LIBATAPP_MACRO_PROTOCOL_USE_DYNAMIC_LIBRARY FALSE)
  endif()
endif()

if(LIBATAPP_MACRO_PROTOCOL_USE_DYNAMIC_LIBRARY)
  add_library("${PROJECT_LIBATAPP_LIB_LINK}-protocol" SHARED ${PROJECT_LIBATAPP_PROTOCOL_SRC_LIST})
  project_build_tools_set_shared_library_declaration(LIBATAPP_MACRO_PROTOCOL_API
                                                     "${PROJECT_LIBATAPP_LIB_LINK}-protocol")
else()
  add_library("${PROJECT_LIBATAPP_LIB_LINK}-protocol" STATIC ${PROJECT_LIBATAPP_PROTOCOL_SRC_LIST})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|Intel|XL|XLClang")
    target_compile_definitions("${PROJECT_LIBATAPP_LIB_LINK}-protocol"
                               PUBLIC "LIBATAPP_MACRO_PROTOCOL_API=__attribute__((visibility(\"default\")))")
  else()
    target_compile_definitions("${PROJECT_LIBATAPP_LIB_LINK}-protocol" PUBLIC "LIBATAPP_MACRO_PROTOCOL_API=")
  endif()
endif()
set_target_properties(
  "${PROJECT_LIBATAPP_LIB_LINK}-protocol"
  PROPERTIES C_VISIBILITY_PRESET "hidden"
             CXX_VISIBILITY_PRESET "hidden"
             VERSION ${LIBATAPP_VERSION}
             WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
target_link_libraries("${PROJECT_LIBATAPP_LIB_LINK}-protocol"
                      PUBLIC ${ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_PROTOBUF_LINK_NAME})

target_include_directories(
  "${PROJECT_LIBATAPP_LIB_LINK}-protocol"
  PRIVATE "$<BUILD_INTERFACE:${PROJECT_LIBATAPP_ROOT_INC_DIR}>"
  PUBLIC "$<BUILD_INTERFACE:${ATFRAMEWORK_LIBATBUS_REPO_DIR}/include>"
         "$<BUILD_INTERFACE:${PROJECT_LIBATAPP_GENERATED_DIR}/include>")

set(PROJECT_LIBATAPP_PROTOCOL_SOURCE_COMPILE_OPTIONS ${PROJECT_COMMON_PRIVATE_COMPILE_OPTIONS}
                                                     ${PROJECT_BUILD_TOOLS_PATCH_PROTOBUF_SOURCES_OPTIONS})
if(COMPILER_STRICT_RECOMMEND_REMOVE_CFLAGS)
  list(REMOVE_ITEM PROJECT_LIBATAPP_PROTOCOL_SOURCE_COMPILE_OPTIONS ${COMPILER_STRICT_RECOMMEND_REMOVE_CFLAGS})
endif()
if(PROJECT_BUILD_TOOLS_PATCH_PROTOBUF_SOURCES_REMOVE_OPTIONS)
  list(REMOVE_ITEM PROJECT_LIBATAPP_PROTOCOL_SOURCE_COMPILE_OPTIONS
       ${PROJECT_BUILD_TOOLS_PATCH_PROTOBUF_SOURCES_REMOVE_OPTIONS})
endif()

if(PROJECT_LIBATAPP_PROTOCOL_SOURCE_COMPILE_OPTIONS)
  target_compile_options("${PROJECT_LIBATAPP_LIB_LINK}-protocol"
                         PRIVATE ${PROJECT_LIBATAPP_PROTOCOL_SOURCE_COMPILE_OPTIONS})
endif()

list(PREPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES "${PROJECT_LIBATAPP_LIB_LINK}-protocol")
list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES ${ATFRAMEWORK_LIBATBUS_LINK_NAME}
     ${ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_YAML_CPP_LINK_NAME})

if(ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_GSL_LINK_NAME)
  list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES ${ATFRAMEWORK_CMAKE_TOOLSET_THIRD_PARTY_GSL_LINK_NAME})
elseif(TARGET Microsoft.GSL::GSL)
  list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES Microsoft.GSL::GSL)
elseif(TARGET gsl::gsl-lite-v1)
  list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES gsl::gsl-lite-v1)
elseif(TARGET gsl::gsl-lite)
  list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES gsl::gsl-lite)
endif()

if(BUILD_SHARED_LIBS OR ATFRAMEWORK_USE_DYNAMIC_LIBRARY)
  add_library(${PROJECT_LIBATAPP_LIB_LINK} SHARED ${PROJECT_LIBATAPP_SRC_LIST})
  target_compile_definitions(${PROJECT_LIBATAPP_LIB_LINK} PRIVATE LIBATAPP_MACRO_API_NATIVE=1 LIBATAPP_MACRO_API_DLL=1)
  set_target_properties(
    ${PROJECT_LIBATAPP_LIB_LINK}
    PROPERTIES VERSION ${LIBATAPP_VERSION}
               SOVERSION ${LIBATAPP_VERSION}
               INTERFACE_COMPILE_DEFINITIONS LIBATAPP_MACRO_API_DLL=1)
else()
  add_library(${PROJECT_LIBATAPP_LIB_LINK} STATIC ${PROJECT_LIBATAPP_SRC_LIST})
  target_compile_definitions(${PROJECT_LIBATAPP_LIB_LINK} PRIVATE LIBATAPP_MACRO_API_NATIVE=1)
  set_target_properties(${PROJECT_LIBATAPP_LIB_LINK} PROPERTIES VERSION ${LIBATAPP_VERSION})
endif()

set_target_properties(${PROJECT_LIBATAPP_LIB_LINK} PROPERTIES C_VISIBILITY_PRESET "hidden" CXX_VISIBILITY_PRESET
                                                                                           "hidden")

if(TARGET rapidjson)
  list(APPEND PROJECT_LIBATAPP_PUBLIC_LINK_NAMES rapidjson)
elseif(RapidJSON_INCLUDE_DIRS)
  list(APPEND PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS ${RapidJSON_INCLUDE_DIRS})
endif()

target_link_libraries(${PROJECT_LIBATAPP_LIB_LINK} PUBLIC ${PROJECT_LIBATAPP_PUBLIC_LINK_NAMES})

target_include_directories(
  ${PROJECT_LIBATAPP_LIB_LINK}
  PUBLIC "$<BUILD_INTERFACE:${PROJECT_LIBATAPP_PUBLIC_INCLUDE_DIRS}>"
         "$<BUILD_INTERFACE:${PROJECT_LIBATAPP_GENERATED_DIR}/include>" "$<INSTALL_INTERFACE:include>"
  PRIVATE "$<BUILD_INTERFACE:${PROJECT_LIBATAPP_ROOT_SRC_DIR}>")

if(PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS)
  target_compile_options(${PROJECT_LIBATAPP_LIB_LINK} PRIVATE ${PROJECT_LIBATAPP_PRIVATE_COMPILE_OPTIONS})
endif()

if(MSVC)
  set_property(TARGET ${PROJECT_LIBATAPP_LIB_LINK} PROPERTY FOLDER "atframework/atapp")
  set_property(TARGET "${PROJECT_LIBATAPP_LIB_LINK}-protocol" PROPERTY FOLDER "atframework/atapp")
endif()

add_library("atframework::${PROJECT_LIBATAPP_LIB_LINK}-protocol" ALIAS "${PROJECT_LIBATAPP_LIB_LINK}-protocol")
add_library("atframework::${PROJECT_LIBATAPP_LIB_LINK}" ALIAS "${PROJECT_LIBATAPP_LIB_LINK}")

install(
  TARGETS "${PROJECT_LIBATAPP_LIB_LINK}" "${PROJECT_LIBATAPP_LIB_LINK}-protocol"
  EXPORT ${PROJECT_LIBATAPP_EXPORT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(
  DIRECTORY ${PROJECT_LIBATAPP_ROOT_INC_DIR}
  DESTINATION .
  FILES_MATCHING
  REGEX ".+\\.h(pp)?$"
  PATTERN ".svn" EXCLUDE
  PATTERN ".git" EXCLUDE)

install(FILES "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe/atapp_conf.pb.h"
              "${PROJECT_LIBATAPP_GENERATED_DIR}/include/atframe/atapp_config.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/atframe/")
