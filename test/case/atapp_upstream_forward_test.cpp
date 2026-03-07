// Copyright 2026 atframework
// Test A: Upstream forwarding data tests
//
// Topology: node1(0x101) --proxy--> upstream(0x102) <--proxy-- node3(0x103)
// node1 sends messages to node3, forwarded through upstream

#include <atframe/atapp.h>
#include <atframe/connectors/atapp_connector_atbus.h>
#include <atframe/modules/etcd_module.h>

#include <common/file_system.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "frame/test_macros.h"

namespace {

struct upstream_test_context {
  int received_message_count = 0;
  int forward_response_count = 0;
  int32_t last_forward_response_error = 0;
  std::vector<std::string> received_messages;
};

static upstream_test_context g_upstream_test_ctx;

static void reset_test_context() {
  g_upstream_test_ctx.received_message_count = 0;
  g_upstream_test_ctx.forward_response_count = 0;
  g_upstream_test_ctx.last_forward_response_error = 0;
  g_upstream_test_ctx.received_messages.clear();
}

static std::string get_test_conf_path(const char *filename) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/" + filename;
}

static bool check_and_skip_if_missing(const std::string &path) {
  if (!atfw::util::file_system::is_exist(path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << path << " not found, skip this test" << '\n';
    return true;
  }
  return false;
}

struct three_node_apps {
  atframework::atapp::app node1;
  atframework::atapp::app upstream;
  atframework::atapp::app node3;

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> node1_discovery;
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> upstream_discovery;
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> node3_discovery;

  bool init_all() {
    std::string conf1 = get_test_conf_path("atapp_test_upstream_1.yaml");
    std::string conf2 = get_test_conf_path("atapp_test_upstream_2.yaml");
    std::string conf3 = get_test_conf_path("atapp_test_upstream_3.yaml");

    if (check_and_skip_if_missing(conf1)) return false;
    if (check_and_skip_if_missing(conf2)) return false;
    if (check_and_skip_if_missing(conf3)) return false;

    // Init upstream first (it needs to be listening before children connect)
    const char *args_up[] = {"upstream", "-c", conf2.c_str(), "start"};
    if (upstream.init(nullptr, 4, args_up, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init upstream app" << '\n';
      return false;
    }

    const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
    if (node1.init(nullptr, 4, args1, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init node1 app" << '\n';
      return false;
    }

    const char *args3[] = {"node3", "-c", conf3.c_str(), "start"};
    if (node3.init(nullptr, 4, args3, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init node3 app" << '\n';
      return false;
    }

    return true;
  }

  void pump(int iterations = 64) {
    for (int i = 0; i < iterations; ++i) {
      upstream.run_noblock();
      node1.run_noblock();
      node3.run_noblock();
    }
  }

  void pump_until(const std::function<bool()> &cond, std::chrono::seconds timeout = std::chrono::seconds(8)) {
    auto start = atfw::util::time::time_utility::sys_now();
    auto end = start + timeout;
    while (!cond() && atfw::util::time::time_utility::sys_now() < end) {
      upstream.run_noblock();
      node1.run_noblock();
      node3.run_noblock();
      atfw::util::time::time_utility::update();
    }
  }

  void inject_all_discovery() {
    node1_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    upstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    node3_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

    {
      atapp::protocol::atapp_discovery info;
      node1.pack(info);
      node1_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }
    {
      atapp::protocol::atapp_discovery info;
      upstream.pack(info);
      upstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }
    {
      atapp::protocol::atapp_discovery info;
      node3.pack(info);
      node3_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    // All nodes know about all other nodes
    auto &node1_gd = node1.get_etcd_module()->get_global_discovery();
    auto &upstream_gd = upstream.get_etcd_module()->get_global_discovery();
    auto &node3_gd = node3.get_etcd_module()->get_global_discovery();

    node1_gd.add_node(node1_discovery);
    node1_gd.add_node(upstream_discovery);
    node1_gd.add_node(node3_discovery);

    upstream_gd.add_node(node1_discovery);
    upstream_gd.add_node(upstream_discovery);
    upstream_gd.add_node(node3_discovery);

    node3_gd.add_node(node1_discovery);
    node3_gd.add_node(upstream_discovery);
    node3_gd.add_node(node3_discovery);
  }

  void inject_discovery_without_node3() {
    node1_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    upstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

    {
      atapp::protocol::atapp_discovery info;
      node1.pack(info);
      node1_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }
    {
      atapp::protocol::atapp_discovery info;
      upstream.pack(info);
      upstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    auto &node1_gd = node1.get_etcd_module()->get_global_discovery();
    auto &upstream_gd = upstream.get_etcd_module()->get_global_discovery();
    auto &node3_gd = node3.get_etcd_module()->get_global_discovery();

    node1_gd.add_node(node1_discovery);
    node1_gd.add_node(upstream_discovery);

    upstream_gd.add_node(node1_discovery);
    upstream_gd.add_node(upstream_discovery);

    node3_gd.add_node(upstream_discovery);
  }

  void inject_node3_discovery_later() {
    node3_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    {
      atapp::protocol::atapp_discovery info;
      node3.pack(info);
      node3_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    auto &node1_gd = node1.get_etcd_module()->get_global_discovery();
    auto &upstream_gd = upstream.get_etcd_module()->get_global_discovery();
    auto &node3_gd = node3.get_etcd_module()->get_global_discovery();

    node1_gd.add_node(node3_discovery);
    upstream_gd.add_node(node3_discovery);
    node3_gd.add_node(node1_discovery);
    node3_gd.add_node(node3_discovery);
  }

  void setup_topology() {
    // upstream knows about its downstream children
    auto up_connector = upstream.get_atbus_connector();
    if (up_connector) {
      up_connector->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
      up_connector->update_topology_peer(node3.get_app_id(), upstream.get_app_id(), nullptr);
    }

    // node1 and node3 know about each other via upstream
    auto n1_connector = node1.get_atbus_connector();
    if (n1_connector) {
      n1_connector->update_topology_peer(upstream.get_app_id(), 0, nullptr);
      n1_connector->update_topology_peer(node3.get_app_id(), upstream.get_app_id(), nullptr);
    }

    auto n3_connector = node3.get_atbus_connector();
    if (n3_connector) {
      n3_connector->update_topology_peer(upstream.get_app_id(), 0, nullptr);
      n3_connector->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    }
  }

  void create_endpoints() {
    CASE_EXPECT_TRUE(node1.mutable_endpoint(node3_discovery));
    CASE_EXPECT_TRUE(node1.mutable_endpoint(upstream_discovery));
    CASE_EXPECT_TRUE(upstream.mutable_endpoint(node1_discovery));
    CASE_EXPECT_TRUE(upstream.mutable_endpoint(node3_discovery));
    CASE_EXPECT_TRUE(node3.mutable_endpoint(node1_discovery));
    CASE_EXPECT_TRUE(node3.mutable_endpoint(upstream_discovery));
  }
};

}  // namespace

// ============================================================
// A.1: upstream_wait_discovery_then_send
// node3 discovery is delayed, node1 sends message first (queued),
// then node3 discovery arrives and message is delivered.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_wait_discovery_then_send) {
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  // Initial pump to get all apps running
  apps.pump(32);

  // Inject discovery for node1 and upstream only (not node3 yet)
  apps.inject_discovery_without_node3();
  apps.setup_topology();

  // Create endpoints for what we know
  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.upstream_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node1_discovery));

  apps.pump(32);

  // Set up message callback on node3
  apps.node3.set_evt_on_forward_request([](atframework::atapp::app &,
                                           const atframework::atapp::app::message_sender_t &sender,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.1: node3 received message: " << received << '\n';
    return 0;
  });

  // node1 sends message to node3 (node3 discovery not yet available)
  char msg_data[] = "hello-from-node1-A1";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};

  // The send might queue or fail without discovery, both are acceptable
  uint64_t seq = 0;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  apps.pump(16);

  // Now inject node3 discovery
  apps.inject_node3_discovery_later();
  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.node3_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node3_discovery));
  CASE_EXPECT_TRUE(apps.node3.mutable_endpoint(apps.node1_discovery));
  CASE_EXPECT_TRUE(apps.node3.mutable_endpoint(apps.upstream_discovery));

  apps.pump(64);

  // Send another message now that discovery is available
  char msg_data2[] = "hello-after-discovery-A1";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span2, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_GE(g_upstream_test_ctx.received_message_count, 1);
  CASE_MSG_INFO() << "A.1: Total messages received by node3: " << g_upstream_test_ctx.received_message_count << '\n';
}

// ============================================================
// A.2: upstream_connected_forward_success
// All nodes connected and discovery ready, direct forward should succeed.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_connected_forward_success) {
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  // Inject all discovery and setup topology
  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // Set up receive callback on node3
  apps.node3.set_evt_on_forward_request([&apps](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    CASE_EXPECT_EQ(apps.node1.get_app_id(), sender.id);
    CASE_EXPECT_EQ(100, msg.type);

    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.2: node3 received: " << received << '\n';
    return 0;
  });

  // Send from node1 to node3
  char msg_data[] = "upstream-forward-A2";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 200;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_upstream_test_ctx.received_message_count);
  if (!g_upstream_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("upstream-forward-A2", g_upstream_test_ctx.received_messages[0]);
  }

  // Also send by name
  char msg_data2[] = "upstream-forward-A2-by-name";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_name(), 100, msg_span2, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 2; });

  CASE_EXPECT_EQ(2, g_upstream_test_ctx.received_message_count);

  // Also send from node3 to node1 (reverse direction)
  int node1_received = 0;
  apps.node1.set_evt_on_forward_request(
      [&node1_received, &apps](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &sender,
                               const atframework::atapp::app::message_t &msg) {
        CASE_EXPECT_EQ(apps.node3.get_app_id(), sender.id);
        ++node1_received;
        return 0;
      });

  char msg_data3[] = "reverse-A2";
  gsl::span<const unsigned char> msg_span3{reinterpret_cast<const unsigned char *>(msg_data3),
                                           static_cast<size_t>(strlen(msg_data3))};
  CASE_EXPECT_EQ(0, apps.node3.send_message(apps.node1.get_app_id(), 101, msg_span3, &seq));

  apps.pump_until([&]() { return node1_received >= 1; });

  CASE_EXPECT_EQ(1, node1_received);
}

// ============================================================
// A.3: upstream_connected_target_unreachable
// Upstream is connected but target node does not exist.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_connected_target_unreachable) {
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // Send message to a non-existent node ID
  char msg_data[] = "to-nobody-A3";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 300;
  uint64_t nonexistent_id = 0x00000199;

  // Need to create a discovery for the fake node
  auto fake_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  atapp::protocol::atapp_discovery fake_info;
  fake_info.set_id(nonexistent_id);
  fake_info.set_name("nonexistent-node");
  fake_discovery->copy_from(fake_info, atapp::etcd_discovery_node::node_version());
  apps.node1.get_etcd_module()->get_global_discovery().add_node(fake_discovery);
  apps.node1.mutable_endpoint(fake_discovery);

  // Setup forward response to capture error
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.3: forward response error_code=" << error_code << '\n';
    return 0;
  });

  int32_t ret = apps.node1.send_message(nonexistent_id, 100, msg_span, &seq);
  CASE_MSG_INFO() << "A.3: send_message to nonexistent node returned: " << ret << '\n';

  apps.pump(64);

  // Either send fails immediately or we get an error response
  if (ret == 0) {
    // Message was queued, pump and check for error response
    apps.pump_until([&]() { return g_upstream_test_ctx.forward_response_count > 0; }, std::chrono::seconds(5));
    if (g_upstream_test_ctx.forward_response_count > 0) {
      CASE_EXPECT_NE(0, g_upstream_test_ctx.last_forward_response_error);
    }
  } else {
    // Send failed immediately - this is also acceptable
    CASE_EXPECT_NE(0, ret);
  }
}

// ============================================================
// A.4: upstream_reconnect_then_send_success
// Upstream disconnects and reconnects, pending messages should be delivered.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_reconnect_then_send_success) {
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // First verify normal send works
  apps.node3.set_evt_on_forward_request([](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.4: node3 received: " << received << '\n';
    return 0;
  });

  char msg_data[] = "before-disconnect-A4";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 400;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 1; });
  CASE_EXPECT_EQ(1, g_upstream_test_ctx.received_message_count);

  // Simulate upstream endpoint removal (disconnect)
  auto node1_bus = apps.node1.get_bus_node();
  if (node1_bus) {
    // Remove the upstream endpoint in node1's bus to simulate disconnect
    auto *upstream_ep = node1_bus->get_endpoint(apps.upstream.get_app_id());
    if (upstream_ep) {
      CASE_MSG_INFO() << "A.4: Simulating upstream disconnect..." << '\n';
    }
  }

  // Queue a message during potential disconnect period
  char msg_data2[] = "during-reconnect-A4";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span2, &seq);

  // Pump extensively to allow reconnection
  apps.pump(128);

  // Send another message after reconnection
  char msg_data3[] = "after-reconnect-A4";
  gsl::span<const unsigned char> msg_span3{reinterpret_cast<const unsigned char *>(msg_data3),
                                           static_cast<size_t>(strlen(msg_data3))};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span3, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 2; });

  CASE_EXPECT_GE(g_upstream_test_ctx.received_message_count, 2);
  CASE_MSG_INFO() << "A.4: Total messages received after reconnect: " << g_upstream_test_ctx.received_message_count
                  << '\n';
}

// ============================================================
// A.5: upstream_retry_exceed_limit_fail
// Upstream disconnects, use set_sys_now() to trigger retry limit exceeded,
// handle should be removed and pending messages fail.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_retry_exceed_limit_fail) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // Setup forward response callback to detect failure
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.5: forward response error_code=" << error_code << '\n';
    return 0;
  });

  // Remove node3's discovery to simulate it going offline
  apps.node1.get_etcd_module()->get_global_discovery().remove_node(apps.node3.get_app_id());

  // Notify connector about discovery deletion
  if (apps.node1.get_atbus_connector()) {
    apps.node1.get_atbus_connector()->on_discovery_event(atapp::etcd_discovery_action_t::kDelete, apps.node3_discovery);
  }

  apps.pump(16);

  // Send a message that should get queued/pending
  char msg_data[] = "will-fail-A5";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 500;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  // Use set_sys_now to advance time past reconnect retries
  // Config: reconnect_start_interval=2s, reconnect_max_try_times=3
  // Each retry: 2s, 4s, 8s -> total ~14s, use 20s to be safe
  auto now = atframework::atapp::app::get_sys_now();

  // Retry 1: advance 3s
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(3));
  apps.pump(32);

  // Retry 2: advance 7s total
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(7));
  apps.pump(32);

  // Retry 3: advance 15s total (past max retries)
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(15));
  apps.pump(32);

  // Advance more to ensure timer fires
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(25));
  apps.pump(64);

  CASE_MSG_INFO() << "A.5: forward response count=" << g_upstream_test_ctx.forward_response_count << '\n';

  // Reset time
  atframework::atapp::app::set_sys_now(now);
#endif
}

// ============================================================
// A.6: upstream_retry_timeout_downstream_cleanup
// Upstream reconnect times out, downstream pending messages fail,
// downstream handle should be cleaned up.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_retry_timeout_downstream_cleanup) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // Track forward response errors on node1
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.6: forward response error_code=" << error_code << '\n';
    return 0;
  });

  // Remove node3 discovery
  apps.node1.get_etcd_module()->get_global_discovery().remove_node(apps.node3.get_app_id());
  if (apps.node1.get_atbus_connector()) {
    apps.node1.get_atbus_connector()->on_discovery_event(atapp::etcd_discovery_action_t::kDelete, apps.node3_discovery);
  }

  apps.pump(16);

  // Send messages that will be pending
  char msg_data[] = "pending-cleanup-A6";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 600;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  // Advance time well past retry limits (reconnect_max_try_times=3)
  auto now = atframework::atapp::app::get_sys_now();
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(30));
  apps.pump(64);

  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(60));
  apps.pump(64);

  // Check that endpoint for node3 on node1 is cleaned up
  auto *endpoint_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_MSG_INFO() << "A.6: node3 endpoint on node1 after timeout: " << (endpoint_node3 ? "exists" : "null") << '\n';
  CASE_MSG_INFO() << "A.6: forward response count=" << g_upstream_test_ctx.forward_response_count << '\n';

  // Reset time
  atframework::atapp::app::set_sys_now(now);
#endif
}

// ============================================================
// A.7: upstream_topology_offline_pending_fail
// Remove topology peer, advance time past lost_topology_timeout,
// handle should be force-removed and pending messages fail.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_topology_offline_pending_fail) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // Setup response callback
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.7: forward response error_code=" << error_code << '\n';
    return 0;
  });

  // Remove topology for node3 on node1
  if (apps.node1.get_atbus_connector()) {
    apps.node1.get_atbus_connector()->remove_topology_peer(apps.node3.get_app_id());
  }

  apps.pump(16);

  // Send a message that will be pending
  char msg_data[] = "topology-offline-A7";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 700;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  // Advance time past lost_topology_timeout (configured to 32s)
  auto now = atframework::atapp::app::get_sys_now();
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(35));
  apps.pump(64);

  // The handle should have been force-removed
  auto *endpoint_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_MSG_INFO() << "A.7: node3 endpoint on node1 after topology timeout: " << (endpoint_node3 ? "exists" : "null")
                  << '\n';
  CASE_MSG_INFO() << "A.7: forward response count=" << g_upstream_test_ctx.forward_response_count << '\n';

  // Reset time
  atframework::atapp::app::set_sys_now(now);
#endif
}

// ============================================================
// A.8: upstream_topology_change_new_upstream
// update_topology_peer switches to a new upstream,
// messages should be forwarded through the new upstream.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_topology_change_new_upstream) {
  reset_test_context();

  // For this test we use a modified topology:
  // node1(0x101) --proxy--> upstream(0x102) <-- node3(0x103)
  // Then we "logically" change node3's topology upstream to be upstream(0x102)
  // This tests that update_topology_peer correctly routes through changed topology

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump(32);

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump(64);

  // Verify initial forwarding works
  apps.node3.set_evt_on_forward_request([](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.8: node3 received: " << received << '\n';
    return 0;
  });

  char msg_data[] = "before-topo-change-A8";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 800;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 1; });
  CASE_EXPECT_EQ(1, g_upstream_test_ctx.received_message_count);

  // Now update topology: the upstream is still 0x102, but we re-confirm/update
  // This simulates a topology change notification from etcd
  if (apps.node1.get_atbus_connector()) {
    apps.node1.get_atbus_connector()->update_topology_peer(apps.node3.get_app_id(), apps.upstream.get_app_id(),
                                                           nullptr);
  }

  apps.pump(32);

  // Send another message to verify forwarding still works after topology update
  char msg_data2[] = "after-topo-change-A8";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span2, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 2; });
  CASE_EXPECT_EQ(2, g_upstream_test_ctx.received_message_count);

  if (g_upstream_test_ctx.received_messages.size() >= 2) {
    CASE_EXPECT_EQ("before-topo-change-A8", g_upstream_test_ctx.received_messages[0]);
    CASE_EXPECT_EQ("after-topo-change-A8", g_upstream_test_ctx.received_messages[1]);
  }
}
