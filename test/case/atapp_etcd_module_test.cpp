// Copyright 2026 atframework
// etcd module integration tests (requires etcd service)

#include <atframe/atapp.h>
#include <atframe/modules/etcd_module.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_discovery.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

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

  // Try to connect to etcd health endpoint via curl
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

// Helper: run apps in noblock mode for a number of iterations
static void run_apps_noblock(std::vector<atframework::atapp::app *> &apps, int iterations) {
  for (int i = 0; i < iterations; ++i) {
    for (auto *app_ptr : apps) {
      app_ptr->run_noblock();
    }
  }
}

// Helper: run apps until a condition is met or timeout
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

}  // namespace

// ============================================================
// I.4.1: etcd_module_init_and_ready
// ============================================================
CASE_TEST(atapp_etcd_module, init_and_ready) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available at " << get_etcd_host() << ", skip this test"
                    << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  // After init, etcd module should be enabled and the cluster should be available
  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  CASE_EXPECT_TRUE(etcd_mod->is_etcd_enabled());
  CASE_EXPECT_TRUE(etcd_mod->get_raw_etcd_ctx().is_available());

  // Tick a few times to ensure stable state
  std::vector<atframework::atapp::app *> apps = {&app1};
  run_apps_noblock(apps, 32);

  CASE_EXPECT_TRUE(etcd_mod->get_raw_etcd_ctx().is_available());
  CASE_EXPECT_NE(0, static_cast<int64_t>(etcd_mod->get_raw_etcd_ctx().get_lease()));

  CASE_MSG_INFO() << "etcd lease: " << etcd_mod->get_raw_etcd_ctx().get_lease() << '\n';
}

// ============================================================
// I.4.2: etcd_module_keepalive_paths
// ============================================================
CASE_TEST(atapp_etcd_module, keepalive_paths) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  // Check by_id path
  std::string by_id_path = etcd_mod->get_discovery_by_id_path();
  CASE_EXPECT_FALSE(by_id_path.empty());
  CASE_MSG_INFO() << "by_id_path: " << by_id_path << '\n';
  // Should contain "by_id" and the app name
  CASE_EXPECT_NE(std::string::npos, by_id_path.find("by_id"));
  CASE_EXPECT_NE(std::string::npos, by_id_path.find(app1.get_app_name()));

  // Check by_name path
  std::string by_name_path = etcd_mod->get_discovery_by_name_path();
  CASE_EXPECT_FALSE(by_name_path.empty());
  CASE_MSG_INFO() << "by_name_path: " << by_name_path << '\n';
  CASE_EXPECT_NE(std::string::npos, by_name_path.find("by_name"));
  CASE_EXPECT_NE(std::string::npos, by_name_path.find(app1.get_app_name()));

  // Check topology path
  std::string topology_path = etcd_mod->get_topology_path();
  CASE_EXPECT_FALSE(topology_path.empty());
  CASE_MSG_INFO() << "topology_path: " << topology_path << '\n';
  CASE_EXPECT_NE(std::string::npos, topology_path.find("topology"));
  CASE_EXPECT_NE(std::string::npos, topology_path.find(app1.get_app_name()));
}

// ============================================================
// I.4.3: etcd_module_watcher_paths
// ============================================================
CASE_TEST(atapp_etcd_module, watcher_paths) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  // Watcher paths should be prefix paths (without node-specific suffix)
  std::string by_id_watcher = etcd_mod->get_discovery_by_id_watcher_path();
  std::string by_name_watcher = etcd_mod->get_discovery_by_name_watcher_path();
  std::string topology_watcher = etcd_mod->get_topology_watcher_path();

  CASE_EXPECT_FALSE(by_id_watcher.empty());
  CASE_EXPECT_FALSE(by_name_watcher.empty());
  CASE_EXPECT_FALSE(topology_watcher.empty());

  CASE_MSG_INFO() << "by_id_watcher_path: " << by_id_watcher << '\n';
  CASE_MSG_INFO() << "by_name_watcher_path: " << by_name_watcher << '\n';
  CASE_MSG_INFO() << "topology_watcher_path: " << topology_watcher << '\n';

  CASE_EXPECT_NE(std::string::npos, by_id_watcher.find("by_id"));
  CASE_EXPECT_NE(std::string::npos, by_name_watcher.find("by_name"));
  CASE_EXPECT_NE(std::string::npos, topology_watcher.find("topology"));

  // Watcher paths should NOT contain the app name (they watch the whole prefix)
  // The keepalive paths should be longer (have the app-specific suffix)
  CASE_EXPECT_LT(by_id_watcher.size(), etcd_mod->get_discovery_by_id_path().size());
  CASE_EXPECT_LT(by_name_watcher.size(), etcd_mod->get_discovery_by_name_path().size());
  CASE_EXPECT_LT(topology_watcher.size(), etcd_mod->get_topology_path().size());
}

// ============================================================
// I.4.5: etcd_module_topology_registration
// ============================================================
CASE_TEST(atapp_etcd_module, topology_registration) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  // Tick to let watcher events come in
  std::vector<atframework::atapp::app *> apps = {&app1};

  bool has_topology = run_apps_until(apps, [&etcd_mod, &app1]() {
    const auto &topo_set = etcd_mod->get_topology_info_set();
    return topo_set.find(app1.get_id()) != topo_set.end();
  });

  CASE_EXPECT_TRUE(has_topology);
  if (has_topology) {
    const auto &topo_set = etcd_mod->get_topology_info_set();
    auto it = topo_set.find(app1.get_id());
    CASE_EXPECT_TRUE(it != topo_set.end());
    if (it != topo_set.end()) {
      CASE_EXPECT_TRUE(!!it->second.info);
      if (it->second.info) {
        CASE_EXPECT_EQ(app1.get_id(), it->second.info->id());
        CASE_MSG_INFO() << "topology info id: " << it->second.info->id() << ", name: " << it->second.info->name()
                        << '\n';
      }
    }
  }
}

// ============================================================
// I.4.14: etcd_module_discovery_snapshot
// ============================================================
CASE_TEST(atapp_etcd_module, discovery_snapshot) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  std::vector<atframework::atapp::app *> apps = {&app1};
  bool has_snapshot = run_apps_until(apps, [&etcd_mod]() { return etcd_mod->has_discovery_snapshot(); });

  CASE_EXPECT_TRUE(has_snapshot);
  CASE_MSG_INFO() << "has_discovery_snapshot: " << (etcd_mod->has_discovery_snapshot() ? "true" : "false") << '\n';
}

// ============================================================
// I.4.15: etcd_module_topology_snapshot
// ============================================================
CASE_TEST(atapp_etcd_module, topology_snapshot) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args[] = {"app1", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  std::vector<atframework::atapp::app *> apps = {&app1};
  bool has_snapshot = run_apps_until(apps, [&etcd_mod]() { return etcd_mod->has_topology_snapshot(); });

  CASE_EXPECT_TRUE(has_snapshot);
  CASE_MSG_INFO() << "has_topology_snapshot: " << (etcd_mod->has_topology_snapshot() ? "true" : "false") << '\n';
}

// ============================================================
// I.4.4: etcd_module_discovery_registration (two nodes)
// ============================================================
CASE_TEST(atapp_etcd_module, discovery_registration) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  atframework::atapp::app app2;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  auto etcd_mod2 = app2.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  CASE_EXPECT_TRUE(!!etcd_mod2);
  if (!etcd_mod1 || !etcd_mod2) {
    return;
  }

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};

  // Wait for node1 to discover node2 through etcd watcher events
  bool node1_found_node2 = run_apps_until(apps, [&etcd_mod1, &app2]() {
    auto node = etcd_mod1->get_global_discovery().get_node_by_id(app2.get_id());
    return !!node;
  });

  CASE_EXPECT_TRUE(node1_found_node2);
  if (node1_found_node2) {
    auto node = etcd_mod1->get_global_discovery().get_node_by_id(app2.get_id());
    CASE_EXPECT_TRUE(!!node);
    if (node) {
      CASE_EXPECT_EQ(app2.get_id(), node->get_discovery_info().id());
      CASE_EXPECT_EQ(app2.get_app_name(), node->get_discovery_info().name());
      CASE_MSG_INFO() << "node1 discovered node2: id=" << node->get_discovery_info().id()
                      << " name=" << node->get_discovery_info().name() << '\n';
    }
  }

  // Also check node2 discovers node1
  bool node2_found_node1 = run_apps_until(apps, [&etcd_mod2, &app1]() {
    auto node = etcd_mod2->get_global_discovery().get_node_by_id(app1.get_id());
    return !!node;
  });

  CASE_EXPECT_TRUE(node2_found_node1);
  if (node2_found_node1) {
    auto node = etcd_mod2->get_global_discovery().get_node_by_id(app1.get_id());
    CASE_EXPECT_TRUE(!!node);
    if (node) {
      CASE_EXPECT_EQ(app1.get_id(), node->get_discovery_info().id());
      CASE_MSG_INFO() << "node2 discovered node1: id=" << node->get_discovery_info().id() << '\n';
    }
  }

  // Also verify by_name lookup works
  {
    auto node_by_name = etcd_mod1->get_global_discovery().get_node_by_name(app2.get_app_name());
    CASE_EXPECT_TRUE(!!node_by_name);
    if (node_by_name) {
      CASE_EXPECT_EQ(app2.get_app_name(), node_by_name->get_discovery_info().name());
      CASE_MSG_INFO() << "node1 discovered node2 by name: " << node_by_name->get_discovery_info().name() << '\n';
    }
  }
}

// ============================================================
// I.4.6: etcd_module_discovery_event_callback (kPut)
// ============================================================
CASE_TEST(atapp_etcd_module, discovery_event_callback) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Register discovery event callback on node1
  int discovery_callback_count = 0;
  atapp::etcd_discovery_node::ptr_t last_discovery_node;
  atapp::etcd_module::node_action_t last_action = atapp::etcd_module::node_action_t::kUnknown;

  auto handle = etcd_mod1->add_on_node_discovery_event(
      [&discovery_callback_count, &last_discovery_node, &last_action](atapp::etcd_module::node_action_t action,
                                                                      const atapp::etcd_discovery_node::ptr_t &node) {
        ++discovery_callback_count;
        last_action = action;
        last_discovery_node = node;
      });

  // Wait for snapshot to complete first
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_discovery_snapshot(); });

  // Now start node2
  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};

  // Wait for discovery callback to fire for node2
  bool callback_fired = run_apps_until(apps, [&discovery_callback_count]() { return discovery_callback_count > 0; });

  CASE_EXPECT_TRUE(callback_fired);
  CASE_EXPECT_GT(discovery_callback_count, 0);
  CASE_EXPECT_TRUE(!!last_discovery_node);

  if (last_discovery_node) {
    CASE_MSG_INFO() << "discovery callback: action=" << static_cast<int>(last_action)
                    << " node_id=" << last_discovery_node->get_discovery_info().id() << '\n';
  }

  // Cleanup
  etcd_mod1->remove_on_node_event(handle);
}

// ============================================================
// I.4.7: etcd_module_discovery_event_remove_callback
// ============================================================
CASE_TEST(atapp_etcd_module, discovery_event_remove_callback) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Register and immediately remove the callback
  int removed_callback_count = 0;
  auto handle = etcd_mod1->add_on_node_discovery_event(
      [&removed_callback_count](atapp::etcd_module::node_action_t, const atapp::etcd_discovery_node::ptr_t &) {
        ++removed_callback_count;
      });

  etcd_mod1->remove_on_node_event(handle);
  int removed_baseline = removed_callback_count;

  // Register a second callback that should still fire
  int active_callback_count = 0;
  auto active_handle = etcd_mod1->add_on_node_discovery_event(
      [&active_callback_count](atapp::etcd_module::node_action_t, const atapp::etcd_discovery_node::ptr_t &) {
        ++active_callback_count;
      });

  // Wait for snapshot, then start node2
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_discovery_snapshot(); });

  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};
  run_apps_until(apps, [&active_callback_count]() { return active_callback_count > 0; });

  // Removed callback should NOT have been called after removal
  CASE_EXPECT_EQ(removed_baseline, removed_callback_count);
  // Active callback should have been called
  CASE_EXPECT_GT(active_callback_count, 0);

  CASE_MSG_INFO() << "removed_callback_count: " << removed_callback_count
                  << ", active_callback_count: " << active_callback_count << '\n';

  // Cleanup
  etcd_mod1->remove_on_node_event(active_handle);
}

// ============================================================
// I.4.9: etcd_module_discovery_event_multi_callbacks
// ============================================================
CASE_TEST(atapp_etcd_module, discovery_event_multi_callbacks) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Register multiple callbacks and track call order
  std::vector<int> call_order;
  int cb1_count = 0;
  int cb2_count = 0;
  int cb3_count = 0;

  auto h1 = etcd_mod1->add_on_node_discovery_event(
      [&cb1_count, &call_order](atapp::etcd_module::node_action_t, const atapp::etcd_discovery_node::ptr_t &) {
        ++cb1_count;
        call_order.push_back(1);
      });

  auto h2 = etcd_mod1->add_on_node_discovery_event(
      [&cb2_count, &call_order](atapp::etcd_module::node_action_t, const atapp::etcd_discovery_node::ptr_t &) {
        ++cb2_count;
        call_order.push_back(2);
      });

  auto h3 = etcd_mod1->add_on_node_discovery_event(
      [&cb3_count, &call_order](atapp::etcd_module::node_action_t, const atapp::etcd_discovery_node::ptr_t &) {
        ++cb3_count;
        call_order.push_back(3);
      });

  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_discovery_snapshot(); });

  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};
  run_apps_until(apps, [&cb1_count]() { return cb1_count > 0; });

  CASE_EXPECT_GT(cb1_count, 0);
  CASE_EXPECT_GT(cb2_count, 0);
  CASE_EXPECT_GT(cb3_count, 0);

  // Check call order: within each event, callbacks should be called in registration order 1,2,3
  if (call_order.size() >= 3) {
    // Find first complete triplet
    CASE_EXPECT_EQ(1, call_order[0]);
    CASE_EXPECT_EQ(2, call_order[1]);
    CASE_EXPECT_EQ(3, call_order[2]);
    CASE_MSG_INFO() << "Multi-callback order verified: " << call_order[0] << "," << call_order[1] << ","
                    << call_order[2] << '\n';
  }

  // Cleanup
  etcd_mod1->remove_on_node_event(h1);
  etcd_mod1->remove_on_node_event(h2);
  etcd_mod1->remove_on_node_event(h3);
}

// ============================================================
// I.4.10: etcd_module_topology_event_callback (kPut)
// ============================================================
CASE_TEST(atapp_etcd_module, topology_event_callback) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Register topology event callback
  int topology_callback_count = 0;
  atapp::etcd_module::atapp_topology_info_ptr_t last_topology_info;
  atapp::etcd_module::topology_action_t last_topology_action = atapp::etcd_watch_event::kPut;
  atapp::etcd_data_version last_version{};

  auto handle = etcd_mod1->add_on_topology_info_event(
      [&topology_callback_count, &last_topology_info, &last_topology_action, &last_version](
          atapp::etcd_module::topology_action_t action, const atapp::etcd_module::atapp_topology_info_ptr_t &info,
          const atapp::etcd_data_version &ver) {
        ++topology_callback_count;
        last_topology_action = action;
        last_topology_info = info;
        last_version = ver;
      });

  // Wait for snapshot to load
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_topology_snapshot(); });

  // Record baseline count (self-registration may fire)
  int baseline_count = topology_callback_count;

  // Start node2
  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};
  bool callback_fired = run_apps_until(
      apps, [&topology_callback_count, baseline_count]() { return topology_callback_count > baseline_count; });

  CASE_EXPECT_TRUE(callback_fired);
  CASE_EXPECT_TRUE(!!last_topology_info);

  if (last_topology_info) {
    CASE_MSG_INFO() << "topology callback: action=" << static_cast<int>(last_topology_action)
                    << " id=" << last_topology_info->id() << " version.create_revision=" << last_version.create_revision
                    << '\n';
    CASE_EXPECT_GT(last_version.create_revision, 0);
  }

  // Cleanup
  etcd_mod1->remove_on_topology_info_event(handle);
}

// ============================================================
// I.4.11: etcd_module_topology_event_remove_callback
// ============================================================
CASE_TEST(atapp_etcd_module, topology_event_remove_callback) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Register and remove topology callback
  int removed_callback_count = 0;
  auto removed_handle = etcd_mod1->add_on_topology_info_event(
      [&removed_callback_count](atapp::etcd_module::topology_action_t,
                                const atapp::etcd_module::atapp_topology_info_ptr_t &,
                                const atapp::etcd_data_version &) { ++removed_callback_count; });

  etcd_mod1->remove_on_topology_info_event(removed_handle);
  int removed_baseline = removed_callback_count;

  // Register active callback
  int active_callback_count = 0;
  auto active_handle = etcd_mod1->add_on_topology_info_event(
      [&active_callback_count](atapp::etcd_module::topology_action_t,
                               const atapp::etcd_module::atapp_topology_info_ptr_t &,
                               const atapp::etcd_data_version &) { ++active_callback_count; });

  // Wait for snapshot
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_topology_snapshot(); });

  int baseline_active = active_callback_count;

  // Start node2
  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};
  run_apps_until(apps, [&active_callback_count, baseline_active]() { return active_callback_count > baseline_active; });

  // Removed callback should NOT have been called after removal
  // Active callback should have been called for node2 topology
  CASE_EXPECT_EQ(removed_baseline, removed_callback_count);
  CASE_EXPECT_GT(active_callback_count, baseline_active);

  CASE_MSG_INFO() << "removed_topology_callback_count: " << removed_callback_count
                  << ", active_topology_callback_count: " << active_callback_count << '\n';

  // Cleanup
  etcd_mod1->remove_on_topology_info_event(active_handle);
}

// ============================================================
// I.4.13: etcd_module_topology_event_multi_callbacks
// ============================================================
CASE_TEST(atapp_etcd_module, topology_event_multi_callbacks) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Register multiple topology callbacks
  int cb1_count = 0;
  int cb2_count = 0;

  auto h1 = etcd_mod1->add_on_topology_info_event([&cb1_count](atapp::etcd_module::topology_action_t,
                                                               const atapp::etcd_module::atapp_topology_info_ptr_t &,
                                                               const atapp::etcd_data_version &) { ++cb1_count; });

  auto h2 = etcd_mod1->add_on_topology_info_event([&cb2_count](atapp::etcd_module::topology_action_t,
                                                               const atapp::etcd_module::atapp_topology_info_ptr_t &,
                                                               const atapp::etcd_data_version &) { ++cb2_count; });

  // Wait for snapshot
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_topology_snapshot(); });

  int baseline1 = cb1_count;
  int baseline2 = cb2_count;

  // Start node2
  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};
  run_apps_until(apps, [&cb1_count, baseline1]() { return cb1_count > baseline1; });

  CASE_EXPECT_GT(cb1_count, baseline1);
  CASE_EXPECT_GT(cb2_count, baseline2);
  // Both callbacks should be called the same number of times for the same events
  CASE_EXPECT_EQ(cb1_count - baseline1, cb2_count - baseline2);

  CASE_MSG_INFO() << "topology multi-callback: cb1=" << (cb1_count - baseline1) << " cb2=" << (cb2_count - baseline2)
                  << '\n';

  // Cleanup
  etcd_mod1->remove_on_topology_info_event(h1);
  etcd_mod1->remove_on_topology_info_event(h2);
}

// ============================================================
// I.5.3: multi_node_topology_update
// ============================================================
CASE_TEST(atapp_etcd_module, multi_node_topology_update) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  atframework::atapp::app app2;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  auto etcd_mod2 = app2.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  CASE_EXPECT_TRUE(!!etcd_mod2);
  if (!etcd_mod1 || !etcd_mod2) {
    return;
  }

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};

  // Wait for node1 to see node2's topology
  bool node1_has_node2_topo = run_apps_until(apps, [&etcd_mod1, &app2]() {
    const auto &topo = etcd_mod1->get_topology_info_set();
    return topo.find(app2.get_id()) != topo.end();
  });

  CASE_EXPECT_TRUE(node1_has_node2_topo);
  if (node1_has_node2_topo) {
    const auto &topo = etcd_mod1->get_topology_info_set();
    auto it = topo.find(app2.get_id());
    CASE_EXPECT_TRUE(it != topo.end());
    if (it != topo.end() && it->second.info) {
      CASE_EXPECT_EQ(app2.get_id(), it->second.info->id());
      CASE_MSG_INFO() << "node1 sees node2 topology: id=" << it->second.info->id() << '\n';
    }
  }

  // Check node2 sees node1's topology
  bool node2_has_node1_topo = run_apps_until(apps, [&etcd_mod2, &app1]() {
    const auto &topo = etcd_mod2->get_topology_info_set();
    return topo.find(app1.get_id()) != topo.end();
  });

  CASE_EXPECT_TRUE(node2_has_node1_topo);
  if (node2_has_node1_topo) {
    const auto &topo = etcd_mod2->get_topology_info_set();
    auto it = topo.find(app1.get_id());
    CASE_EXPECT_TRUE(it != topo.end());
    if (it != topo.end() && it->second.info) {
      CASE_EXPECT_EQ(app1.get_id(), it->second.info->id());
      CASE_MSG_INFO() << "node2 sees node1 topology: id=" << it->second.info->id() << '\n';
    }
  }
}

// ============================================================
// I.4.8: etcd_module_discovery_event_delete
// ============================================================
CASE_TEST(atapp_etcd_module, discovery_event_delete) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Track discovery events
  int put_count = 0;
  int delete_count = 0;
  atapp::etcd_module::node_action_t last_action = atapp::etcd_module::node_action_t::kUnknown;

  auto handle = etcd_mod1->add_on_node_discovery_event(
      [&put_count, &delete_count, &last_action](atapp::etcd_module::node_action_t action,
                                                const atapp::etcd_discovery_node::ptr_t &) {
        last_action = action;
        if (action == atapp::etcd_module::node_action_t::kPut) {
          ++put_count;
        } else if (action == atapp::etcd_module::node_action_t::kDelete) {
          ++delete_count;
        }
      });

  // Wait for snapshot on node1
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_discovery_snapshot(); });

  // Start node2 and wait for kPut event
  {
    atframework::atapp::app app2;
    const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
    CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

    auto etcd_mod2 = app2.get_etcd_module();
    CASE_EXPECT_TRUE(!!etcd_mod2);
    if (!etcd_mod2) {
      etcd_mod1->remove_on_node_event(handle);
      return;
    }

    std::vector<atframework::atapp::app *> apps = {&app1, &app2};
    bool got_put = run_apps_until(apps, [&put_count]() { return put_count > 0; });
    CASE_EXPECT_TRUE(got_put);
    CASE_MSG_INFO() << "discovery_event_delete: got kPut, put_count=" << put_count << '\n';

    // Now stop node2's etcd module to revoke its lease, triggering kDelete
    etcd_mod2->stop();

    // Tick both apps to process the close/revoke HTTP request
    run_apps_noblock(apps, 30);
  }
  // app2 is destroyed here

  // Continue ticking app1 to receive the kDelete watcher event
  bool got_delete = run_apps_until(apps1, [&delete_count]() { return delete_count > 0; }, std::chrono::seconds(20));

  CASE_EXPECT_TRUE(got_delete);
  CASE_EXPECT_GT(delete_count, 0);
  CASE_MSG_INFO() << "discovery_event_delete: put_count=" << put_count << " delete_count=" << delete_count << '\n';

  // Cleanup
  etcd_mod1->remove_on_node_event(handle);
}

// ============================================================
// I.4.12: etcd_module_topology_event_delete
// ============================================================
CASE_TEST(atapp_etcd_module, topology_event_delete) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Track topology events
  int put_count = 0;
  int delete_count = 0;

  auto handle = etcd_mod1->add_on_topology_info_event(
      [&put_count, &delete_count](atapp::etcd_module::topology_action_t action,
                                  const atapp::etcd_module::atapp_topology_info_ptr_t &,
                                  const atapp::etcd_data_version &) {
        if (action == atapp::etcd_watch_event::kPut) {
          ++put_count;
        } else if (action == atapp::etcd_watch_event::kDelete) {
          ++delete_count;
        }
      });

  // Wait for snapshot on node1
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_topology_snapshot(); });

  // Start node2 and wait for kPut event
  {
    atframework::atapp::app app2;
    const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
    CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

    auto etcd_mod2 = app2.get_etcd_module();
    CASE_EXPECT_TRUE(!!etcd_mod2);
    if (!etcd_mod2) {
      etcd_mod1->remove_on_topology_info_event(handle);
      return;
    }

    std::vector<atframework::atapp::app *> apps = {&app1, &app2};
    bool got_put = run_apps_until(apps, [&put_count]() { return put_count > 0; });
    CASE_EXPECT_TRUE(got_put);
    CASE_MSG_INFO() << "topology_event_delete: got kPut, put_count=" << put_count << '\n';

    // Stop node2's etcd module to revoke its lease
    etcd_mod2->stop();
    run_apps_noblock(apps, 30);
  }

  // Continue ticking app1 to receive the kDelete watcher event
  bool got_delete = run_apps_until(apps1, [&delete_count]() { return delete_count > 0; }, std::chrono::seconds(20));

  CASE_EXPECT_TRUE(got_delete);
  CASE_EXPECT_GT(delete_count, 0);
  CASE_MSG_INFO() << "topology_event_delete: put_count=" << put_count << " delete_count=" << delete_count << '\n';

  // Cleanup
  etcd_mod1->remove_on_topology_info_event(handle);
}

// ============================================================
// I.4.16: etcd_module_stop_revoke_lease
// ============================================================
CASE_TEST(atapp_etcd_module, stop_revoke_lease) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app;
  const char *args[] = {"app", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  // Wait for module to be ready and get a lease
  std::vector<atframework::atapp::app *> apps = {&app};
  bool ready = run_apps_until(apps, [&etcd_mod]() { return etcd_mod->get_raw_etcd_ctx().is_available(); });
  CASE_EXPECT_TRUE(ready);

  int64_t lease_before = etcd_mod->get_raw_etcd_ctx().get_lease();
  CASE_EXPECT_NE(0, lease_before);
  CASE_MSG_INFO() << "stop_revoke_lease: lease before stop = " << lease_before << '\n';

  // Stop the etcd module - this should revoke the lease
  etcd_mod->stop();

  // Tick to process the close/revoke request
  bool stopped = run_apps_until(
      apps,
      [&etcd_mod]() {
        // After stop completes, the context should no longer be available
        return !etcd_mod->get_raw_etcd_ctx().is_available();
      },
      std::chrono::seconds(10));

  CASE_EXPECT_TRUE(stopped);
  CASE_MSG_INFO() << "stop_revoke_lease: etcd ctx available after stop = "
                  << (etcd_mod->get_raw_etcd_ctx().is_available() ? "true" : "false") << '\n';
}

// ============================================================
// I.4.17: etcd_module_reload_config
// ============================================================
CASE_TEST(atapp_etcd_module, reload_config) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app;
  const char *args[] = {"app", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  // Wait for module to be ready
  std::vector<atframework::atapp::app *> apps = {&app};
  bool ready = run_apps_until(apps, [&etcd_mod]() { return etcd_mod->get_raw_etcd_ctx().is_available(); });
  CASE_EXPECT_TRUE(ready);

  // Verify reload succeeds
  int reload_ret = app.reload();
  CASE_EXPECT_EQ(0, reload_ret);
  CASE_MSG_INFO() << "reload_config: app.reload() returned " << reload_ret << '\n';

  // Verify etcd module is still functional after reload
  CASE_EXPECT_TRUE(etcd_mod->is_etcd_enabled());
  CASE_EXPECT_TRUE(etcd_mod->get_raw_etcd_ctx().is_available());

  // Tick a few times to verify no issues after reload
  run_apps_noblock(apps, 5);

  CASE_EXPECT_TRUE(etcd_mod->get_raw_etcd_ctx().is_available());
  CASE_MSG_INFO() << "reload_config: etcd module still available after reload" << '\n';
}

// ============================================================
// I.4.18: etcd_module_disable_enable
// ============================================================
CASE_TEST(atapp_etcd_module, disable_enable) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_etcd.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app;
  const char *args[] = {"app", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app.init(nullptr, 4, args, nullptr));

  auto etcd_mod = app.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod);
  if (!etcd_mod) {
    return;
  }

  // Wait for module to be ready
  std::vector<atframework::atapp::app *> apps = {&app};
  bool ready = run_apps_until(apps, [&etcd_mod]() { return etcd_mod->get_raw_etcd_ctx().is_available(); });
  CASE_EXPECT_TRUE(ready);
  CASE_EXPECT_TRUE(etcd_mod->is_etcd_enabled());

  // Disable etcd
  etcd_mod->disable_etcd();
  CASE_EXPECT_FALSE(etcd_mod->is_etcd_enabled());
  CASE_MSG_INFO() << "disable_enable: after disable, is_etcd_enabled="
                  << (etcd_mod->is_etcd_enabled() ? "true" : "false") << '\n';

  // Tick a few times while disabled - the module should start closing
  run_apps_noblock(apps, 10);

  // Re-enable etcd
  etcd_mod->enable_etcd();
  CASE_EXPECT_TRUE(etcd_mod->is_etcd_enabled());
  CASE_MSG_INFO() << "disable_enable: after enable, is_etcd_enabled="
                  << (etcd_mod->is_etcd_enabled() ? "true" : "false") << '\n';

  // Tick to let it recover
  run_apps_noblock(apps, 10);
}

// ============================================================
// I.5.1: multi_node_discovery_put_event
// ============================================================
CASE_TEST(atapp_etcd_module, multi_node_discovery_put_event) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  // Wait for snapshot
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  run_apps_until(apps1, [&etcd_mod1]() { return etcd_mod1->has_discovery_snapshot(); });

  // Start node2
  atframework::atapp::app app2;
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  // Register callback to track kPut events specifically for node2's ID
  uint64_t target_id = app2.get_id();
  bool got_node2_put = false;
  int put_event_count = 0;
  auto handle = etcd_mod1->add_on_node_discovery_event(
      [&got_node2_put, &put_event_count, target_id](atapp::etcd_module::node_action_t action,
                                                    const atapp::etcd_discovery_node::ptr_t &node) {
        if (action == atapp::etcd_module::node_action_t::kPut && node) {
          ++put_event_count;
          if (node->get_discovery_info().id() == target_id) {
            got_node2_put = true;
          }
        }
      });

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};

  // Wait for node1 to receive node2's discovery kPut event
  bool success = run_apps_until(apps, [&got_node2_put]() { return got_node2_put; });
  CASE_EXPECT_TRUE(success);
  CASE_EXPECT_TRUE(got_node2_put);
  CASE_EXPECT_GT(put_event_count, 0);

  // Also verify global_discovery contains node2
  auto node = etcd_mod1->get_global_discovery().get_node_by_id(app2.get_id());
  CASE_EXPECT_TRUE(!!node);

  CASE_MSG_INFO() << "multi_node_discovery_put_event: put_count=" << put_event_count
                  << " got_node2=" << (got_node2_put ? "true" : "false") << '\n';

  // Cleanup
  etcd_mod1->remove_on_node_event(handle);
}

// ============================================================
// I.5.2: multi_node_discovery_delete_event
// ============================================================
CASE_TEST(atapp_etcd_module, multi_node_discovery_delete_event) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  if (!etcd_mod1) {
    return;
  }

  uint64_t app2_id = 0;

  // Start node2 and wait for node1 to discover it
  {
    atframework::atapp::app app2;
    const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
    CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));
    app2_id = app2.get_id();

    auto etcd_mod2 = app2.get_etcd_module();
    CASE_EXPECT_TRUE(!!etcd_mod2);
    if (!etcd_mod2) {
      return;
    }

    std::vector<atframework::atapp::app *> apps = {&app1, &app2};

    // Wait for node1 to discover node2
    bool found = run_apps_until(
        apps, [&etcd_mod1, app2_id]() { return !!etcd_mod1->get_global_discovery().get_node_by_id(app2_id); });
    CASE_EXPECT_TRUE(found);
    CASE_MSG_INFO() << "multi_node_discovery_delete: node1 found node2, id=" << app2_id << '\n';

    // Stop node2's etcd module to revoke lease
    etcd_mod2->stop();
    run_apps_noblock(apps, 30);
  }

  // Continue ticking app1 - node2 should disappear from global_discovery
  std::vector<atframework::atapp::app *> apps1 = {&app1};
  bool node2_gone = run_apps_until(
      apps1, [&etcd_mod1, app2_id]() { return !etcd_mod1->get_global_discovery().get_node_by_id(app2_id); },
      std::chrono::seconds(20));

  CASE_EXPECT_TRUE(node2_gone);
  CASE_MSG_INFO() << "multi_node_discovery_delete: node2 removed from global_discovery = "
                  << (node2_gone ? "true" : "false") << '\n';
}

// ============================================================
// I.5.4: multi_node_custom_data
//   Note: app::pack() currently has custom_data commented out (atapp.cpp),
//   so cross-node propagation is not enabled. This test verifies the local
//   set/get API and the update flag mechanism instead.
// ============================================================
CASE_TEST(atapp_etcd_module, multi_node_custom_data) {
  if (!is_etcd_available()) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd is not available, skip this test" << '\n';
    return;
  }

  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_etcd_node1.yaml";
  std::string conf_path_2 = conf_path_base + "/atapp_test_etcd_node2.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str()) ||
      !atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "etcd node config not found, skip this test" << '\n';
    return;
  }

  atframework::atapp::app app1;
  atframework::atapp::app app2;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  const char *args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  auto etcd_mod1 = app1.get_etcd_module();
  auto etcd_mod2 = app2.get_etcd_module();
  CASE_EXPECT_TRUE(!!etcd_mod1);
  CASE_EXPECT_TRUE(!!etcd_mod2);
  if (!etcd_mod1 || !etcd_mod2) {
    return;
  }

  std::vector<atframework::atapp::app *> apps = {&app1, &app2};

  // Wait for mutual discovery
  bool mutual_discovery = run_apps_until(apps, [&etcd_mod1, &etcd_mod2, &app1, &app2]() {
    return !!etcd_mod1->get_global_discovery().get_node_by_id(app2.get_id()) &&
           !!etcd_mod2->get_global_discovery().get_node_by_id(app1.get_id());
  });
  CASE_EXPECT_TRUE(mutual_discovery);

  // Verify set/get custom_data API works locally
  const std::string custom_data_value = "test-custom-data-12345";
  CASE_EXPECT_TRUE(etcd_mod1->get_conf_custom_data().empty());

  etcd_mod1->set_conf_custom_data(custom_data_value);
  CASE_EXPECT_EQ(custom_data_value, etcd_mod1->get_conf_custom_data());

  // Trigger the keepalive discovery value update flag
  etcd_mod1->set_maybe_update_keepalive_discovery_value();

  // Tick to process the update
  run_apps_noblock(apps, 10);

  // Verify custom_data is still set after ticking
  CASE_EXPECT_EQ(custom_data_value, etcd_mod1->get_conf_custom_data());

  // Verify the discovery value was refreshed (node2 should still see node1 - the update
  // triggers a keepalive value regeneration, though custom_data is not packed into it
  // since app::pack() currently has custom_data commented out)
  auto node = etcd_mod2->get_global_discovery().get_node_by_id(app1.get_id());
  CASE_EXPECT_TRUE(!!node);

  // Verify custom_data can be updated and cleared
  etcd_mod1->set_conf_custom_data("updated-data");
  CASE_EXPECT_EQ("updated-data", etcd_mod1->get_conf_custom_data());

  etcd_mod1->set_conf_custom_data("");
  CASE_EXPECT_TRUE(etcd_mod1->get_conf_custom_data().empty());

  CASE_MSG_INFO() << "multi_node_custom_data: local set/get API verified" << '\n';
}
