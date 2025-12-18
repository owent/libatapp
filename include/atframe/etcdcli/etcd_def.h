// Copyright 2021 atframework
// Created by owent on 2017-11-17

#pragma once

#include <stdint.h>
#include <string>

#include <atframe/atapp_config.h>
#include <config/compile_optimize.h>

LIBATAPP_MACRO_NAMESPACE_BEGIN
struct LIBATAPP_MACRO_API_HEAD_ONLY etcd_response_header {
  uint64_t cluster_id;
  uint64_t member_id;
  int64_t revision;
  uint64_t raft_term;
};

struct LIBATAPP_MACRO_API_HEAD_ONLY etcd_key_value {
  std::string key;
  int64_t create_revision;
  int64_t mod_revision;
  int64_t version;
  std::string value;
  int64_t lease;
};

struct LIBATAPP_MACRO_API_HEAD_ONLY etcd_watch_event {
  enum type {
    EN_WEVT_PUT = 0,    // put
    EN_WEVT_DELETE = 1  // delete
  };
};
LIBATAPP_MACRO_NAMESPACE_END
