// Copyright 2021 atframework
// Created by owent on 2016-04-23

#pragma once

#include <config/compiler/protobuf_prefix.h>

#include <yaml-cpp/yaml.h>

#include <atframe/atapp_conf.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/timestamp.pb.h>

#include <config/compiler/protobuf_suffix.h>

#include <libatbus_protocol.h>

#include <libatbus.h>

#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "atframe/atapp_config.h"

namespace util {
namespace config {
class ini_value;
}
}  // namespace util

namespace atapp {
struct app_conf {
  // bus configure
  std::string id_cmd;
  atbus::node::bus_id_t id;
  std::vector<atbus::node::bus_id_t> id_mask;  // convert a.b.c.d -> id
  std::string conf_file;
  std::string pid_file;
  const char *execute_path;
  bool upgrade_mode;

  atbus::node::conf_t bus_conf;
  std::string app_version;
  std::string hash_code;

  std::list<std::string> startup_log;

  atapp::protocol::atapp_configure origin;
  atapp::protocol::atapp_log log;
  atapp::protocol::atapp_metadata metadata;
};

enum ATAPP_ERROR_TYPE {
  EN_ATAPP_ERR_SUCCESS = 0,
  EN_ATAPP_ERR_NOT_INITED = -1001,
  EN_ATAPP_ERR_ALREADY_INITED = -1002,
  EN_ATAPP_ERR_WRITE_PID_FILE = -1003,
  EN_ATAPP_ERR_SETUP_TIMER = -1004,
  EN_ATAPP_ERR_ALREADY_CLOSED = -1005,
  EN_ATAPP_ERR_MISSING_CONFIGURE_FILE = -1006,
  EN_ATAPP_ERR_LOAD_CONFIGURE_FILE = -1007,
  EN_ATAPP_ERR_OPERATION_TIMEOUT = -1008,
  EN_ATAPP_ERR_RECURSIVE_CALL= -1009,
  EN_ATAPP_ERR_SETUP_ATBUS = -1101,
  EN_ATAPP_ERR_SEND_FAILED = -1102,
  EN_ATAPP_ERR_DISCOVERY_DISABLED = -1103,
  EN_ATAPP_ERR_COMMAND_IS_NULL = -1801,
  EN_ATAPP_ERR_NO_AVAILABLE_ADDRESS = -1802,
  EN_ATAPP_ERR_CONNECT_ATAPP_FAILED = -1803,
  EN_ATAPP_ERR_MIN = -1999,
};

LIBATAPP_MACRO_API void parse_timepoint(const std::string &in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out);
LIBATAPP_MACRO_API void parse_duration(const std::string &in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out);

LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst);
LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                           std::string prefix);

LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst);
LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node &src,
                                            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                            std::string prefix);
LIBATAPP_MACRO_API const YAML::Node yaml_loader_get_child_by_path(const YAML::Node &src, const std::string &path);

LIBATAPP_MACRO_API bool protobuf_equal(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r);
}  // namespace atapp
