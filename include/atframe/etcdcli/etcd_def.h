// Copyright 2026 atframework
//
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

enum class etcd_watch_event : int32_t {
  kPut = 0,    // put
  kDelete = 1  // delete
};

struct LIBATAPP_MACRO_API_SYMBOL_VISIBLE etcd_data_version {
  int64_t create_revision;
  // Causion: modify_revision of multiple key may be same when they are modifiedx in one transaction of etcd
  // @see https://etcd.io/docs/v3.6/learning/api_guarantees/#revision
  int64_t modify_revision;
  int64_t version;

  ATFW_UTIL_FORCEINLINE etcd_data_version() : create_revision(0), modify_revision(0), version(0) {}
  ATFW_UTIL_FORCEINLINE etcd_data_version(const etcd_data_version &other)
      : create_revision(other.create_revision), modify_revision(other.modify_revision), version(other.version) {}
  ATFW_UTIL_FORCEINLINE etcd_data_version(etcd_data_version &&other)
      : create_revision(other.create_revision), modify_revision(other.modify_revision), version(other.version) {}

  ATFW_UTIL_FORCEINLINE etcd_data_version &operator=(const etcd_data_version &other) {
    create_revision = other.create_revision;
    modify_revision = other.modify_revision;
    version = other.version;
    return *this;
  }

  ATFW_UTIL_FORCEINLINE etcd_data_version &operator=(etcd_data_version &&other) {
    create_revision = other.create_revision;
    modify_revision = other.modify_revision;
    version = other.version;
    return *this;
  }
};

LIBATAPP_MACRO_NAMESPACE_END
