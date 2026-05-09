// Copyright 2026 atframework
// Unit tests for etcd_watcher helper logic (no etcd service required)

// clang-format off
#include <config/compiler/protobuf_prefix.h>
// clang-format on

#include <atframe/atapp_conf.pb.h>

// clang-format off
#include <config/compiler/protobuf_suffix.h>
// clang-format on

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <chrono>
#include <cstdint>

#include "frame/test_macros.h"

namespace {
static int64_t duration_to_milliseconds(std::chrono::system_clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
}  // namespace

// ---- J.1 set_conf_from_protobuf: empty config uses documented defaults ----
CASE_TEST(atapp_etcd_watcher_unit, set_conf_from_protobuf_default_values) {
  atapp::etcd_cluster cluster;
  atapp::etcd_watcher::ptr_t watcher = atapp::etcd_watcher::create(cluster, "/unit-test/watcher", "+1");

  CASE_EXPECT_TRUE(static_cast<bool>(watcher));
  if (!watcher) {
    return;
  }

  atapp::protocol::atapp_etcd_watcher config;
  watcher->set_conf_from_protobuf(config);

  CASE_EXPECT_EQ(3600000LL, duration_to_milliseconds(watcher->get_conf_request_timeout()));
  CASE_EXPECT_EQ(15000LL, duration_to_milliseconds(watcher->get_conf_retry_interval()));
  CASE_EXPECT_EQ(180000LL, duration_to_milliseconds(watcher->get_conf_get_request_timeout()));
  CASE_EXPECT_EQ(0LL, duration_to_milliseconds(watcher->get_conf_startup_random_delay_min()));
  CASE_EXPECT_EQ(0LL, duration_to_milliseconds(watcher->get_conf_startup_random_delay_max()));
}

// ---- J.2 set_conf_from_protobuf: configured durations map into watcher state ----
CASE_TEST(atapp_etcd_watcher_unit, set_conf_from_protobuf_custom_values) {
  atapp::etcd_cluster cluster;
  atapp::etcd_watcher::ptr_t watcher = atapp::etcd_watcher::create(cluster, "/unit-test/watcher", "+1");

  CASE_EXPECT_TRUE(static_cast<bool>(watcher));
  if (!watcher) {
    return;
  }

  atapp::protocol::atapp_etcd_watcher config;
  config.mutable_request_timeout()->set_seconds(12);
  config.mutable_request_timeout()->set_nanos(345000000);
  config.mutable_retry_interval()->set_seconds(6);
  config.mutable_retry_interval()->set_nanos(789000000);
  config.mutable_get_request_timeout()->set_seconds(45);
  config.mutable_get_request_timeout()->set_nanos(67000000);
  config.mutable_startup_random_delay_min()->set_nanos(250000000);
  config.mutable_startup_random_delay_max()->set_seconds(2);
  config.mutable_startup_random_delay_max()->set_nanos(500000000);

  watcher->set_conf_from_protobuf(config);

  CASE_EXPECT_EQ(12345LL, duration_to_milliseconds(watcher->get_conf_request_timeout()));
  CASE_EXPECT_EQ(6789LL, duration_to_milliseconds(watcher->get_conf_retry_interval()));
  CASE_EXPECT_EQ(45067LL, duration_to_milliseconds(watcher->get_conf_get_request_timeout()));
  CASE_EXPECT_EQ(250LL, duration_to_milliseconds(watcher->get_conf_startup_random_delay_min()));
  CASE_EXPECT_EQ(2500LL, duration_to_milliseconds(watcher->get_conf_startup_random_delay_max()));
}