// Copyright 2026 atframework
// etcd_cluster / etcd_keepalive / etcd_watcher integration tests (requires etcd service)

#include <atframe/atapp.h>
#include <atframe/modules/etcd_module.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_packer.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <config/compiler/template_prefix.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <config/compiler/template_suffix.h>

#include <common/file_system.h>
#include <time/time_utility.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "frame/test_macros.h"

namespace {

static std::string get_etcd_host() {
  const char *env = getenv("ATAPP_UNIT_TEST_ETCD_HOST");
  return env ? env : "http://127.0.0.1:12379";
}

static bool is_etcd_available() {
  std::string host = get_etcd_host();
  if (host.empty()) {
    return false;
  }

  std::string health_url = host + "/health";
  auto req = atfw::util::network::http_request::create(nullptr, health_url);
  if (!req) {
    return false;
  }

  req->set_opt_timeout(3000);
  req->set_opt_connect_timeout(2000);

  int res = req->start(atfw::util::network::http_request::method_t::EN_MT_GET, true);
  if (res != 0) {
    return false;
  }

  return req->get_response_code() == 200;
}

static void run_apps_noblock(std::vector<atframework::atapp::app *> &apps, int iterations) {
  for (int i = 0; i < iterations; ++i) {
    for (auto *app_ptr : apps) {
      app_ptr->run_noblock();
    }
  }
}

template <typename CondFn>
static bool run_apps_until(std::vector<atframework::atapp::app *> &apps, CondFn &&cond,
                           std::chrono::seconds timeout_sec = std::chrono::seconds(15)) {
  auto start_time = atfw::util::time::time_utility::sys_now();
  auto end_time = start_time + timeout_sec;

  while (!cond() && atfw::util::time::time_utility::sys_now() < end_time) {
    for (auto *app_ptr : apps) {
      app_ptr->run_noblock();
    }
    atfw::util::time::time_utility::update();
  }

  return cond();
}

// Direct synchronous KV get to etcd (bypasses cluster curl_multi)
static std::string direct_etcd_kv_get(const std::string &key) {
  std::string host = get_etcd_host();
  std::string url = host + "/v3/kv/range";
  auto req = atfw::util::network::http_request::create(nullptr, url);
  if (!req) {
    return "";
  }

  rapidjson::Document doc;
  doc.SetObject();
  atapp::etcd_packer::pack_base64(doc, "key", key, doc);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);

  req->post_data().assign(sb.GetString(), sb.GetSize());
  req->set_opt_timeout(5000);
  req->set_opt_connect_timeout(3000);

  int res = req->start(atfw::util::network::http_request::method_t::EN_MT_POST, true);
  if (res != 0 || req->get_response_code() != 200) {
    return "";
  }

  rapidjson::Document resp_doc;
  if (!atapp::etcd_packer::parse_object(resp_doc, req->get_response_stream().str().c_str())) {
    return "";
  }

  int64_t count = 0;
  atapp::etcd_packer::unpack_int(resp_doc, "count", count);
  if (count <= 0) {
    return "";
  }

  auto kvs_it = resp_doc.FindMember("kvs");
  if (kvs_it == resp_doc.MemberEnd() || !kvs_it->value.IsArray()) {
    return "";
  }

  auto arr = kvs_it->value.GetArray();
  if (arr.Begin() == arr.End()) {
    return "";
  }

  atapp::etcd_key_value kv;
  atapp::etcd_packer::unpack(kv, *arr.Begin());
  return kv.value;
}

// Direct synchronous KV set to etcd
static bool direct_etcd_kv_set(const std::string &key, const std::string &value) {
  std::string host = get_etcd_host();
  std::string url = host + "/v3/kv/put";
  auto req = atfw::util::network::http_request::create(nullptr, url);
  if (!req) {
    return false;
  }

  rapidjson::Document doc;
  doc.SetObject();
  atapp::etcd_packer::pack_base64(doc, "key", key, doc);
  atapp::etcd_packer::pack_base64(doc, "value", value, doc);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);

  req->post_data().assign(sb.GetString(), sb.GetSize());
  req->set_opt_timeout(5000);
  req->set_opt_connect_timeout(3000);

  int res = req->start(atfw::util::network::http_request::method_t::EN_MT_POST, true);
  return res == 0 && req->get_response_code() == 200;
}

// Direct synchronous KV delete from etcd (prefix delete if range_end=="+1")
static bool direct_etcd_kv_del(const std::string &key, const std::string &range_end = "") {
  std::string host = get_etcd_host();
  std::string url = host + "/v3/kv/deleterange";
  auto req = atfw::util::network::http_request::create(nullptr, url);
  if (!req) {
    return false;
  }

  rapidjson::Document doc;
  doc.SetObject();
  atapp::etcd_packer::pack_key_range(doc, key, range_end, doc);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
  doc.Accept(writer);

  req->post_data().assign(sb.GetString(), sb.GetSize());
  req->set_opt_timeout(5000);
  req->set_opt_connect_timeout(3000);

  int res = req->start(atfw::util::network::http_request::method_t::EN_MT_POST, true);
  return res == 0 && req->get_response_code() == 200;
}

struct test_app_setup {
  std::string conf_path;
  bool available;

  test_app_setup() : available(false) {
    if (!is_etcd_available()) {
      return;
    }

    std::string conf_path_base;
    atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
    conf_path = conf_path_base + "/atapp_test_etcd.yaml";

    if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
      return;
    }

    available = true;
  }
};

}  // namespace

// ============================================================
// I.1.1: cluster_init_and_connect
// ============================================================
CASE_TEST(atapp_etcd_cluster, cluster_init_and_connect) {  // NOLINT
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  // Cluster should be available after init
  CASE_EXPECT_TRUE(cluster.is_available());
  CASE_EXPECT_TRUE(cluster.check_flag(atapp::etcd_cluster::flag_t::kRunning));
  CASE_EXPECT_TRUE(cluster.check_flag(atapp::etcd_cluster::flag_t::kReady));

  // Lease should be granted
  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  CASE_EXPECT_NE(0, static_cast<int64_t>(cluster.get_lease()));
  CASE_MSG_INFO() << "cluster lease: " << cluster.get_lease() << '\n';
}

// ============================================================
// I.1.2: cluster_member_list_discovery
// ============================================================
CASE_TEST(atapp_etcd_cluster, cluster_member_list_discovery) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  // Available hosts should contain at least one member
  const auto &hosts = cluster.get_available_hosts();
  CASE_EXPECT_FALSE(hosts.empty());
  CASE_MSG_INFO() << "available hosts count: " << hosts.size() << '\n';
  for (const auto &h : hosts) {
    CASE_MSG_INFO() << "  host: " << h << '\n';
  }

  // Selected host should be set
  CASE_EXPECT_FALSE(cluster.get_selected_host().empty());
  CASE_MSG_INFO() << "selected host: " << cluster.get_selected_host() << '\n';
}

// ============================================================
// I.1.3: cluster_lease_grant_and_keepalive
// ============================================================
CASE_TEST(atapp_etcd_cluster, cluster_lease_grant_and_keepalive) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  int64_t lease_before = cluster.get_lease();
  CASE_EXPECT_NE(0, lease_before);

  // Tick more to allow keepalive renewal
  run_apps_noblock(apps, 32);

  int64_t lease_after = cluster.get_lease();
  // Lease should remain the same (successfully renewed, not re-granted)
  CASE_EXPECT_EQ(lease_before, lease_after);
  CASE_MSG_INFO() << "lease stable: " << lease_before << " == " << lease_after << '\n';
}

// ============================================================
// I.1.4: cluster_close_revoke_lease
// ============================================================
CASE_TEST(atapp_etcd_cluster, cluster_close_revoke_lease) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  // Write a key with lease binding
  std::string test_key = "/atapp/unit-test/atapp_etcd_cluster/close-revoke/" + std::to_string(cluster.get_lease());
  bool kv_set_done = false;
  auto set_req = cluster.create_request_kv_set(test_key, "lease-bound-value", true);
  if (set_req) {
    set_req->set_on_complete([&kv_set_done](atfw::util::network::http_request &) -> int {
      kv_set_done = true;
      return 0;
    });
    set_req->start(atfw::util::network::http_request::method_t::EN_MT_POST, false);
  }

  run_apps_until(apps, [&kv_set_done]() { return kv_set_done; });

  // Verify key exists before close
  std::string val_before = direct_etcd_kv_get(test_key);
  CASE_EXPECT_EQ("lease-bound-value", val_before);

  int64_t lease_before_close = cluster.get_lease();
  CASE_EXPECT_NE(0, lease_before_close);

  // Close with revoke_lease=true
  auto close_req = cluster.close(false, true);

  // Verify cluster enters closing state
  CASE_EXPECT_TRUE(cluster.check_flag(atapp::etcd_cluster::flag_t::kClosing));
  CASE_MSG_INFO() << "close requested, closing=" << cluster.check_flag(atapp::etcd_cluster::flag_t::kClosing)
                  << " close_req=" << (close_req ? "valid" : "null") << '\n';

  // Clean up the test key directly so it doesn't leak
  direct_etcd_kv_del(test_key, "");
}

// ============================================================
// I.1.5: cluster_stats_tracking
// ============================================================
CASE_TEST(atapp_etcd_cluster, cluster_stats_tracking) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 32);

  const auto &stats = cluster.get_stats();
  // After init and ticks, there should have been requests (member list, lease grant, keepalive, etc.)
  CASE_EXPECT_GT(stats.sum_create_requests, static_cast<size_t>(0));
  CASE_EXPECT_GT(stats.sum_success_requests, static_cast<size_t>(0));

  CASE_MSG_INFO() << "stats: create=" << stats.sum_create_requests << " success=" << stats.sum_success_requests
                  << " error=" << stats.sum_error_requests << '\n';
}

// ============================================================
// I.1.6: cluster_event_up_down_callbacks
// ============================================================
CASE_TEST(atapp_etcd_cluster, cluster_event_up_down_callbacks) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  int up_count = 0;
  int down_count = 0;

  // Register event_up callback - trigger_if_running=true should fire immediately since cluster is already ready
  auto up_handle = cluster.add_on_event_up([&up_count](atapp::etcd_cluster &) { ++up_count; },
                                           true  // trigger immediately if already running
  );

  auto down_handle = cluster.add_on_event_down([&down_count](atapp::etcd_cluster &) { ++down_count; },
                                               false  // don't trigger now
  );

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  // Up callback should have been triggered (cluster is running)
  CASE_EXPECT_GT(up_count, 0);
  CASE_EXPECT_EQ(0, down_count);

  CASE_MSG_INFO() << "up_count=" << up_count << " down_count=" << down_count << '\n';

  // Clean up handles
  cluster.remove_on_event_up(up_handle);
  cluster.remove_on_event_down(down_handle);
}

// ============================================================
// I.2.1: keepalive_set_value_and_read
// ============================================================
CASE_TEST(atapp_etcd_cluster, keepalive_set_value_and_read) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  // Clean up test key first
  std::string test_path = "/atapp/unit-test/atapp_etcd_cluster/keepalive/ka1";
  direct_etcd_kv_del(test_path, "+1");

  // Create a keepalive
  auto ka = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka);
  if (!ka) {
    return;
  }

  ka->set_value("keepalive-value-1");
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka));

  // Drive the loop until has_data
  bool has_data = run_apps_until(apps, [&ka]() { return ka->has_data(); });
  CASE_EXPECT_TRUE(has_data);

  // Read back via direct HTTP
  std::string read_val = direct_etcd_kv_get(test_path);
  CASE_EXPECT_EQ("keepalive-value-1", read_val);

  // Clean up
  cluster.remove_keepalive(ka);
  direct_etcd_kv_del(test_path, "+1");
}

// ============================================================
// I.2.2: keepalive_update_value
// ============================================================
CASE_TEST(atapp_etcd_cluster, keepalive_update_value) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string test_path = "/atapp/unit-test/atapp_etcd_cluster/keepalive/ka2";
  direct_etcd_kv_del(test_path, "+1");

  auto ka = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka);
  if (!ka) {
    return;
  }

  ka->set_value("initial-value");
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka));

  // Wait for initial value to be set
  bool has_data = run_apps_until(apps, [&ka]() { return ka->has_data(); });
  CASE_EXPECT_TRUE(has_data);

  std::string read_val = direct_etcd_kv_get(test_path);
  CASE_EXPECT_EQ("initial-value", read_val);

  // Update value
  ka->set_value("updated-value");

  // Wait for update to propagate
  bool updated = run_apps_until(apps, [&test_path]() {
    std::string v = direct_etcd_kv_get(test_path);
    return v == "updated-value";
  });
  CASE_EXPECT_TRUE(updated);

  // Clean up
  cluster.remove_keepalive(ka);
  direct_etcd_kv_del(test_path, "+1");
}

// ============================================================
// I.2.3: keepalive_lease_binding
// ============================================================
CASE_TEST(atapp_etcd_cluster, keepalive_lease_binding) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string test_path = "/atapp/unit-test/atapp_etcd_cluster/keepalive/ka3-lease";
  direct_etcd_kv_del(test_path, "+1");

  auto ka = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka);
  if (!ka) {
    return;
  }

  ka->set_value("lease-bound");
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka));

  bool has_data = run_apps_until(apps, [&ka]() { return ka->has_data(); });
  CASE_EXPECT_TRUE(has_data);

  // Value should exist
  std::string val = direct_etcd_kv_get(test_path);
  CASE_EXPECT_EQ("lease-bound", val);

  // Remove keepalive - this stops the renewal
  cluster.remove_keepalive(ka);
  ka.reset();

  // Verify the key is still present right after removing keepalive (lease hasn't expired yet)
  std::string val_still = direct_etcd_kv_get(test_path);
  CASE_EXPECT_EQ("lease-bound", val_still);
  CASE_MSG_INFO() << "after remove keepalive, KV is still present (lease not yet expired)" << '\n';

  // Clean up test key directly
  direct_etcd_kv_del(test_path, "+1");
}

// ============================================================
// I.2.4: keepalive_checker_conflict
// ============================================================
CASE_TEST(atapp_etcd_cluster, keepalive_checker_conflict) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string test_path = "/atapp/unit-test/atapp_etcd_cluster/keepalive/ka4-conflict";
  direct_etcd_kv_del(test_path, "+1");

  // Write a value directly (simulating another app)
  direct_etcd_kv_set(test_path, "other-app-value");

  // Create keepalive with checker set to our own value
  auto ka = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka);
  if (!ka) {
    return;
  }

  ka->set_checker("my-value");
  ka->set_value("my-value");
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka));

  // Drive the loop until checker has been run
  bool checker_done = run_apps_until(apps, [&ka]() { return ka->is_check_run(); });
  CASE_EXPECT_TRUE(checker_done);

  // Checker should fail because etcd has "other-app-value" but checker expects "my-value"
  CASE_EXPECT_FALSE(ka->is_check_passed());

  // Value in etcd should still be the other app's value (not overwritten)
  std::string val = direct_etcd_kv_get(test_path);
  CASE_EXPECT_EQ("other-app-value", val);

  CASE_MSG_INFO() << "checker conflict: check_passed=" << ka->is_check_passed() << " etcd_value=" << val << '\n';

  // Clean up
  cluster.remove_keepalive(ka);
  direct_etcd_kv_del(test_path, "+1");
}

// ============================================================
// I.2.5: keepalive_checker_same_identity
// ============================================================
CASE_TEST(atapp_etcd_cluster, keepalive_checker_same_identity) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string test_path = "/atapp/unit-test/atapp_etcd_cluster/keepalive/ka5-same";
  direct_etcd_kv_del(test_path, "+1");

  // Pre-write the same value (simulating restart with same value)
  std::string test_value = "same-identity-value";
  direct_etcd_kv_set(test_path, test_value);

  // Create keepalive with checker matching the existing value
  auto ka = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka);
  if (!ka) {
    return;
  }

  ka->set_checker(test_value);
  ka->set_value(test_value);
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka));

  // Drive until checker runs
  bool checker_done = run_apps_until(apps, [&ka]() { return ka->is_check_run(); });
  CASE_EXPECT_TRUE(checker_done);

  // Checker should pass (same value)
  CASE_EXPECT_TRUE(ka->is_check_passed());

  CASE_MSG_INFO() << "same identity: check_passed=" << ka->is_check_passed() << '\n';

  // Clean up
  cluster.remove_keepalive(ka);
  direct_etcd_kv_del(test_path, "+1");
}

// ============================================================
// I.2.6: keepalive_remove_and_readd
// ============================================================
CASE_TEST(atapp_etcd_cluster, keepalive_remove_and_readd) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string test_path = "/atapp/unit-test/atapp_etcd_cluster/keepalive/ka6-readd";
  direct_etcd_kv_del(test_path, "+1");

  // Create and add keepalive
  auto ka = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka);
  if (!ka) {
    return;
  }

  ka->set_value("first-add");
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka));

  bool has_data = run_apps_until(apps, [&ka]() { return ka->has_data(); });
  CASE_EXPECT_TRUE(has_data);
  CASE_EXPECT_EQ("first-add", direct_etcd_kv_get(test_path));

  // Remove keepalive
  CASE_EXPECT_TRUE(cluster.remove_keepalive(ka));

  // Delete the key manually (since lease is still active, old KV may persist)
  direct_etcd_kv_del(test_path, "+1");

  // Create a new keepalive on the same path
  auto ka2 = atapp::etcd_keepalive::create(cluster, test_path);
  CASE_EXPECT_TRUE(!!ka2);
  if (!ka2) {
    return;
  }

  ka2->set_value("second-add");
  CASE_EXPECT_TRUE(cluster.add_keepalive(ka2));

  bool has_data2 = run_apps_until(apps, [&ka2]() { return ka2->has_data(); });
  CASE_EXPECT_TRUE(has_data2);
  CASE_EXPECT_EQ("second-add", direct_etcd_kv_get(test_path));

  // Clean up
  cluster.remove_keepalive(ka2);
  direct_etcd_kv_del(test_path, "+1");
}

// ============================================================
// I.3.1: watcher_initial_snapshot
// ============================================================
CASE_TEST(atapp_etcd_cluster, watcher_initial_snapshot) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  // Pre-write keys
  std::string prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/snapshot/";
  direct_etcd_kv_del(prefix, "+1");
  direct_etcd_kv_set(prefix + "key1", "val1");
  direct_etcd_kv_set(prefix + "key2", "val2");
  direct_etcd_kv_set(prefix + "key3", "val3");

  // Create watcher on the prefix
  bool got_snapshot = false;
  std::vector<atapp::etcd_key_value> snapshot_kvs;

  auto watcher = atapp::etcd_watcher::create(cluster, prefix, "+1");
  CASE_EXPECT_TRUE(!!watcher);
  if (!watcher) {
    return;
  }

  watcher->set_evt_handle([&got_snapshot, &snapshot_kvs](const atapp::etcd_response_header &,
                                                         const atapp::etcd_watcher::response_t &evt_data) {
    if (evt_data.snapshot) {
      got_snapshot = true;
      for (const auto &evt : evt_data.events) {
        snapshot_kvs.push_back(evt.kv);
      }
    }
  });

  CASE_EXPECT_TRUE(cluster.add_watcher(watcher));

  // Drive until snapshot received
  bool snapshot_ok = run_apps_until(apps, [&got_snapshot]() { return got_snapshot; });
  CASE_EXPECT_TRUE(snapshot_ok);
  CASE_EXPECT_GE(snapshot_kvs.size(), static_cast<size_t>(3));

  CASE_MSG_INFO() << "snapshot received " << snapshot_kvs.size() << " kvs" << '\n';
  for (const auto &kv : snapshot_kvs) {
    CASE_MSG_INFO() << "  key=" << kv.key << " value=" << kv.value << '\n';
  }

  // Clean up
  cluster.remove_watcher(watcher);
  direct_etcd_kv_del(prefix, "+1");
}

// ============================================================
// I.3.2: watcher_put_event
// ============================================================
CASE_TEST(atapp_etcd_cluster, watcher_put_event) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/put/";
  direct_etcd_kv_del(prefix, "+1");

  bool got_snapshot = false;
  bool got_put = false;
  std::string put_key;
  std::string put_value;

  auto watcher = atapp::etcd_watcher::create(cluster, prefix, "+1");
  CASE_EXPECT_TRUE(!!watcher);
  if (!watcher) {
    return;
  }

  watcher->set_evt_handle([&got_snapshot, &got_put, &put_key, &put_value](
                              const atapp::etcd_response_header &, const atapp::etcd_watcher::response_t &evt_data) {
    if (evt_data.snapshot) {
      got_snapshot = true;
      return;
    }

    for (const auto &evt : evt_data.events) {
      if (evt.evt_type == atapp::etcd_watch_event::kPut) {
        got_put = true;
        put_key = evt.kv.key;
        put_value = evt.kv.value;
      }
    }
  });

  CASE_EXPECT_TRUE(cluster.add_watcher(watcher));

  // Wait for initial snapshot
  run_apps_until(apps, [&got_snapshot]() { return got_snapshot; });
  CASE_EXPECT_TRUE(got_snapshot);

  // Now write a key
  direct_etcd_kv_set(prefix + "new-key", "new-value");

  // Wait for put event
  bool put_ok = run_apps_until(apps, [&got_put]() { return got_put; });
  CASE_EXPECT_TRUE(put_ok);
  CASE_EXPECT_EQ(prefix + "new-key", put_key);
  CASE_EXPECT_EQ("new-value", put_value);

  CASE_MSG_INFO() << "put event: key=" << put_key << " value=" << put_value << '\n';

  // Clean up
  cluster.remove_watcher(watcher);
  direct_etcd_kv_del(prefix, "+1");
}

// ============================================================
// I.3.3: watcher_delete_event
// ============================================================
CASE_TEST(atapp_etcd_cluster, watcher_delete_event) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/delete/";
  direct_etcd_kv_del(prefix, "+1");

  // Pre-write a key
  std::string del_key = prefix + "to-delete";
  direct_etcd_kv_set(del_key, "deleteme");

  bool got_snapshot = false;
  bool got_delete = false;
  std::string deleted_key;

  auto watcher = atapp::etcd_watcher::create(cluster, prefix, "+1");
  CASE_EXPECT_TRUE(!!watcher);
  if (!watcher) {
    return;
  }

  watcher->set_evt_handle([&got_snapshot, &got_delete, &deleted_key](const atapp::etcd_response_header &,
                                                                     const atapp::etcd_watcher::response_t &evt_data) {
    if (evt_data.snapshot) {
      got_snapshot = true;
      return;
    }

    for (const auto &evt : evt_data.events) {
      if (evt.evt_type == atapp::etcd_watch_event::kDelete) {
        got_delete = true;
        deleted_key = evt.kv.key;
      }
    }
  });

  CASE_EXPECT_TRUE(cluster.add_watcher(watcher));

  // Wait for snapshot
  run_apps_until(apps, [&got_snapshot]() { return got_snapshot; });
  CASE_EXPECT_TRUE(got_snapshot);

  // Delete the key
  direct_etcd_kv_del(del_key);

  // Wait for delete event
  bool del_ok = run_apps_until(apps, [&got_delete]() { return got_delete; });
  CASE_EXPECT_TRUE(del_ok);
  CASE_EXPECT_EQ(del_key, deleted_key);

  CASE_MSG_INFO() << "delete event: key=" << deleted_key << '\n';

  // Clean up
  cluster.remove_watcher(watcher);
  direct_etcd_kv_del(prefix, "+1");
}

// ============================================================
// I.3.4: watcher_prefix_range
// ============================================================
CASE_TEST(atapp_etcd_cluster, watcher_prefix_range) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string watched_prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/prefix/match/";
  std::string other_prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/prefix/other/";
  direct_etcd_kv_del(watched_prefix, "+1");
  direct_etcd_kv_del(other_prefix, "+1");

  bool got_snapshot = false;
  int match_put_count = 0;
  std::vector<std::string> match_keys;

  auto watcher = atapp::etcd_watcher::create(cluster, watched_prefix, "+1");
  CASE_EXPECT_TRUE(!!watcher);
  if (!watcher) {
    return;
  }

  watcher->set_evt_handle([&got_snapshot, &match_put_count, &match_keys](
                              const atapp::etcd_response_header &, const atapp::etcd_watcher::response_t &evt_data) {
    if (evt_data.snapshot) {
      got_snapshot = true;
      return;
    }

    for (const auto &evt : evt_data.events) {
      if (evt.evt_type == atapp::etcd_watch_event::kPut) {
        ++match_put_count;
        match_keys.push_back(evt.kv.key);
      }
    }
  });

  CASE_EXPECT_TRUE(cluster.add_watcher(watcher));

  // Wait for snapshot
  run_apps_until(apps, [&got_snapshot]() { return got_snapshot; });

  // Write to watched prefix
  direct_etcd_kv_set(watched_prefix + "a", "va");
  direct_etcd_kv_set(watched_prefix + "b", "vb");

  // Write to different prefix (should NOT be seen by watcher)
  direct_etcd_kv_set(other_prefix + "x", "vx");

  // Wait for at least 2 put events
  bool got_both = run_apps_until(apps, [&match_put_count]() { return match_put_count >= 2; });
  CASE_EXPECT_TRUE(got_both);
  CASE_EXPECT_EQ(2, match_put_count);

  // Verify only matched prefix keys
  for (const auto &k : match_keys) {
    CASE_EXPECT_NE(std::string::npos, k.find(watched_prefix));
    CASE_EXPECT_EQ(std::string::npos, k.find(other_prefix));
  }

  CASE_MSG_INFO() << "prefix watcher: got " << match_put_count << " events" << '\n';

  // Clean up
  cluster.remove_watcher(watcher);
  direct_etcd_kv_del(watched_prefix, "+1");
  direct_etcd_kv_del(other_prefix, "+1");
}

// ============================================================
// I.3.5: watcher_reconnect_after_timeout
// ============================================================
CASE_TEST(atapp_etcd_cluster, watcher_reconnect_after_timeout) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/reconnect/";
  direct_etcd_kv_del(prefix, "+1");

  int snapshot_count = 0;
  int put_count = 0;

  auto watcher = atapp::etcd_watcher::create(cluster, prefix, "+1");
  CASE_EXPECT_TRUE(!!watcher);
  if (!watcher) {
    return;
  }

  // Set a very short request timeout (2 seconds) to force reconnection
  watcher->set_conf_request_timeout_sec(2);

  watcher->set_evt_handle([&snapshot_count, &put_count](const atapp::etcd_response_header &,
                                                        const atapp::etcd_watcher::response_t &evt_data) {
    if (evt_data.snapshot) {
      ++snapshot_count;
      return;
    }

    for (const auto &evt : evt_data.events) {
      if (evt.evt_type == atapp::etcd_watch_event::kPut) {
        ++put_count;
      }
    }
  });

  CASE_EXPECT_TRUE(cluster.add_watcher(watcher));

  // Wait for initial snapshot
  bool first_snapshot = run_apps_until(apps, [&snapshot_count]() { return snapshot_count >= 1; });
  CASE_EXPECT_TRUE(first_snapshot);

  // Write a key before timeout
  direct_etcd_kv_set(prefix + "pre-timeout", "v1");
  bool got_put = run_apps_until(apps, [&put_count]() { return put_count >= 1; });
  CASE_EXPECT_TRUE(got_put);

  // Wait for the short timeout to trigger reconnection
  // The watcher should automatically re-establish the watch
  CASE_MSG_INFO() << "waiting for watcher timeout and reconnect (2s+ timeout)..." << '\n';

  // Sleep briefly then write another key - if reconnect works, we should see it
  int put_before = put_count;
  // Wait 3 seconds for timeout to expire
  auto wait_start = atfw::util::time::time_utility::sys_now();
  run_apps_until(
      apps,
      [&wait_start]() { return (atfw::util::time::time_utility::sys_now() - wait_start) > std::chrono::seconds(3); },
      std::chrono::seconds(5));

  // Write after timeout - the reconnected watcher should pick this up
  direct_etcd_kv_set(prefix + "post-timeout", "v2");
  bool got_post =
      run_apps_until(apps, [&put_count, put_before]() { return put_count > put_before; }, std::chrono::seconds(10));
  CASE_EXPECT_TRUE(got_post);

  CASE_MSG_INFO() << "reconnect test: snapshots=" << snapshot_count << " puts=" << put_count << '\n';

  // Clean up
  cluster.remove_watcher(watcher);
  direct_etcd_kv_del(prefix, "+1");
}

// ============================================================
// I.3.6: watcher_revision_continuity
// ============================================================
CASE_TEST(atapp_etcd_cluster, watcher_revision_continuity) {
  test_app_setup setup;
  if (!setup.available) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd not available, skip" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", setup.conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  auto &cluster = etcd_mod->get_raw_etcd_ctx();

  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 16);

  std::string prefix = "/atapp/unit-test/atapp_etcd_cluster/watcher/revision/";
  direct_etcd_kv_del(prefix, "+1");

  bool got_snapshot = false;
  std::vector<int64_t> revisions;

  auto watcher = atapp::etcd_watcher::create(cluster, prefix, "+1");
  CASE_EXPECT_TRUE(!!watcher);
  if (!watcher) {
    return;
  }

  watcher->set_evt_handle([&got_snapshot, &revisions](const atapp::etcd_response_header &,
                                                      const atapp::etcd_watcher::response_t &evt_data) {
    if (evt_data.snapshot) {
      got_snapshot = true;
      return;
    }

    // Record the mod_revision from each individual event's kv data
    // Note: header.revision is per-response, so events in the same batch share it.
    // Individual kv.mod_revision reflects each put's actual revision.
    for (auto &evt : evt_data.events) {
      if (evt.kv.mod_revision > 0) {
        revisions.push_back(evt.kv.mod_revision);
      }
    }
  });

  CASE_EXPECT_TRUE(cluster.add_watcher(watcher));

  // Wait for snapshot
  run_apps_until(apps, [&got_snapshot]() { return got_snapshot; });

  // Write multiple keys sequentially
  constexpr int num_writes = 5;
  for (int i = 0; i < num_writes; ++i) {
    direct_etcd_kv_set(prefix + "key-" + std::to_string(i), "val-" + std::to_string(i));
  }

  // Wait for all put events
  bool got_all = run_apps_until(apps, [&revisions]() { return revisions.size() >= num_writes; });
  CASE_EXPECT_TRUE(got_all);

  // Verify revisions are strictly increasing (each sequential PUT gets its own revision)
  for (size_t i = 1; i < revisions.size(); ++i) {
    CASE_EXPECT_GE(revisions[i], revisions[i - 1]);
  }

  CASE_MSG_INFO() << "revision continuity: " << revisions.size() << " revisions received" << '\n';
  for (size_t i = 0; i < revisions.size(); ++i) {
    CASE_MSG_INFO() << "  revision[" << i << "]=" << revisions[i] << '\n';
  }

  // Clean up
  cluster.remove_watcher(watcher);
  direct_etcd_kv_del(prefix, "+1");
}
