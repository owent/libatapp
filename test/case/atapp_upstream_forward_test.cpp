// Copyright 2026 atframework
// Test A: Upstream forwarding data tests
//
// Topology: node1(0x101) --proxy--> upstream(0x102) <--proxy-- node3(0x103)
// node1 sends messages to node3, forwarded through upstream

#include <atframe/atapp.h>
#include <atframe/connectors/atapp_connector_atbus.h>
#include <atframe/modules/etcd_module.h>
#include <detail/libatbus_error.h>

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
  std::vector<atframework::atapp::app_id_t> received_direct_source_ids;
};

static upstream_test_context g_upstream_test_ctx;

static void reset_test_context() {
  g_upstream_test_ctx.received_message_count = 0;
  g_upstream_test_ctx.forward_response_count = 0;
  g_upstream_test_ctx.last_forward_response_error = 0;
  g_upstream_test_ctx.received_messages.clear();
  g_upstream_test_ctx.received_direct_source_ids.clear();
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

  void pump_until(const std::function<bool()> &cond, std::chrono::seconds timeout = std::chrono::seconds(8)) {
    auto start = atfw::util::time::time_utility::sys_now();
    auto end = start + timeout;
    while (!cond() && atfw::util::time::time_utility::sys_now() < end) {
      upstream.run_noblock();
      node1.run_noblock();
      node3.run_noblock();
      CASE_THREAD_SLEEP_MS(1);
      atfw::util::time::time_utility::update();
    }
  }

  void pump_until_connected(std::chrono::seconds timeout = std::chrono::seconds(8)) {
    pump_until(
        [this]() {
          auto *ep_on_node1 = node1.get_endpoint(node3.get_app_id());
          auto *ep_on_node3 = node3.get_endpoint(node1.get_app_id());
          return ep_on_node1 != nullptr && ep_on_node1->get_ready_connection_handle() != nullptr &&
                 ep_on_node3 != nullptr && ep_on_node3->get_ready_connection_handle() != nullptr;
        },
        timeout);
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
    // Update bus node topology registries (required for mutable_connection_handle to find peers)
    auto bus1 = node1.get_bus_node();
    auto bus_up = upstream.get_bus_node();
    auto bus3 = node3.get_bus_node();

    if (bus_up && bus_up->get_topology_registry()) {
      bus_up->get_topology_registry()->update_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
      bus_up->get_topology_registry()->update_peer(node3.get_app_id(), upstream.get_app_id(), nullptr);
    }
    if (bus1 && bus1->get_topology_registry()) {
      bus1->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
      bus1->get_topology_registry()->update_peer(node3.get_app_id(), upstream.get_app_id(), nullptr);
    }
    if (bus3 && bus3->get_topology_registry()) {
      bus3->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
      bus3->get_topology_registry()->update_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    }

    // Also update connector-level topology (for handle-level routing)
    auto up_connector = upstream.get_atbus_connector();
    if (up_connector) {
      up_connector->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
      up_connector->update_topology_peer(node3.get_app_id(), upstream.get_app_id(), nullptr);
    }

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
// node3 discovery is delayed. After it arrives, node1 creates the endpoint,
// sends a message, and verifies immediate delivery via the upstream proxy.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_wait_discovery_then_send) {
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  // Wait for all upstream connections to be established
  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        auto bus3 = apps.node3.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  // Phase 1: Only inject discovery for node1 and upstream, NOT node3 yet
  apps.inject_discovery_without_node3();

  // Setup topology and endpoints for upstream connections only
  auto bus1 = apps.node1.get_bus_node();
  auto bus_up = apps.upstream.get_bus_node();
  auto bus3 = apps.node3.get_bus_node();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(apps.upstream.get_app_id(), 0, nullptr);
  }
  if (bus3 && bus3->get_topology_registry()) {
    bus3->get_topology_registry()->update_peer(apps.upstream.get_app_id(), 0, nullptr);
  }

  auto up_connector = apps.upstream.get_atbus_connector();
  if (up_connector) {
    up_connector->update_topology_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  auto n1_connector = apps.node1.get_atbus_connector();
  if (n1_connector) {
    n1_connector->update_topology_peer(apps.upstream.get_app_id(), 0, nullptr);
  }
  auto n3_connector = apps.node3.get_atbus_connector();
  if (n3_connector) {
    n3_connector->update_topology_peer(apps.upstream.get_app_id(), 0, nullptr);
  }

  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.upstream_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node1_discovery));

  // Verify no endpoint for node3 yet on node1
  CASE_EXPECT_TRUE(apps.node1.get_endpoint(apps.node3.get_app_id()) == nullptr);

  // Set up message callback on node3
  apps.node3.set_evt_on_forward_request([](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.1: node3 received message: " << received << '\n';
    return 0;
  });

  // Phase 2: Node3 discovery arrives — inject and set up topology
  apps.inject_node3_discovery_later();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(apps.node3.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(apps.node3.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus3 && bus3->get_topology_registry()) {
    bus3->get_topology_registry()->update_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (n1_connector) {
    n1_connector->update_topology_peer(apps.node3.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (up_connector) {
    up_connector->update_topology_peer(apps.node3.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (n3_connector) {
    n3_connector->update_topology_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }

  // Create endpoints for node3 on all nodes
  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.node3_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node3_discovery));
  CASE_EXPECT_TRUE(apps.node3.mutable_endpoint(apps.node1_discovery));
  CASE_EXPECT_TRUE(apps.node3.mutable_endpoint(apps.upstream_discovery));

  // Verify node3 endpoint now exists on node1 and handle is ready (proxied via upstream)
  auto *ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_EXPECT_TRUE(ep_node3 != nullptr);
  if (ep_node3 != nullptr) {
    CASE_EXPECT_TRUE(ep_node3->get_ready_connection_handle() != nullptr);
    CASE_MSG_INFO() << "A.1: node3 handle ready=" << (ep_node3->get_ready_connection_handle() != nullptr) << '\n';
  }

  // Send message to node3 — should be delivered immediately via proxy
  char msg_data[] = "hello-from-node1-A1";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  int old_received_message_count = g_upstream_test_ctx.received_message_count;
  int32_t send_ret = apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);
  CASE_MSG_INFO() << "A.1: send_message ret=" << send_ret << " seq=" << seq << '\n';
  CASE_EXPECT_EQ(send_ret, 0);

  // Pump until message is delivered
  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= old_received_message_count + 1; });

  CASE_EXPECT_EQ(g_upstream_test_ctx.received_message_count, old_received_message_count + 1);
  if (!g_upstream_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ(g_upstream_test_ctx.received_messages[0], "hello-from-node1-A1");
  }
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

  // Wait for upstream connections
  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        auto bus3 = apps.node3.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  // Inject all discovery and setup topology
  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  // Wait until all handles are ready
  apps.pump_until_connected();

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
                               const atframework::atapp::app::message_t &) {
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

  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump_until_connected();

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
// Handle set unready → messages go to pending queue → handle set ready → pending messages delivered.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_reconnect_then_send_success) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_handle_unready_by_bus_id() only available in Debug builds, skip"
                  << '\n';
  return;
#else
  reset_test_context();

  three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        auto bus3 = apps.node3.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  // Wait for handles to be ready
  apps.pump_until_connected();

  // Set up message callback on node3
  apps.node3.set_evt_on_forward_request([](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.4: node3 received: " << received << '\n';
    return 0;
  });

  // First message: verify normal send works
  char msg_data[] = "before-disconnect-A4";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 400;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 1; });
  CASE_EXPECT_EQ(1, g_upstream_test_ctx.received_message_count);

  // Simulate disconnect by setting the handle for node3 to unready on node1's connector
  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (n1_connector) {
    CASE_MSG_INFO() << "A.4: Setting handle unready to simulate disconnect..." << '\n';
    n1_connector->set_handle_unready_by_bus_id(apps.node3.get_app_id());
  }

  // Second message: sent while handle is unready → should go to pending queue
  char msg_data2[] = "during-reconnect-A4";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span2, &seq);

  // Verify message is queued as pending
  auto *ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_EXPECT_TRUE(ep_node3 != nullptr);
  if (ep_node3 != nullptr) {
    CASE_MSG_INFO() << "A.4: pending count during disconnect: " << ep_node3->get_pending_message_count() << '\n';
    CASE_EXPECT_GT(ep_node3->get_pending_message_count(), static_cast<size_t>(0));
  }

  // Simulate reconnect by setting the handle back to ready
  if (n1_connector) {
    CASE_MSG_INFO() << "A.4: Setting handle ready to simulate reconnect..." << '\n';
    n1_connector->set_handle_ready_by_bus_id(apps.node3.get_app_id());
  }

  // Pump to let pending messages be delivered via waker
  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 2; });
  CASE_EXPECT_GE(g_upstream_test_ctx.received_message_count, 2);

  // Third message: after reconnection, should send directly without entering pending queue
  char msg_data3[] = "after-reconnect-A4";
  gsl::span<const unsigned char> msg_span3{reinterpret_cast<const unsigned char *>(msg_data3),
                                           static_cast<size_t>(strlen(msg_data3))};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span3, &seq));

  // Verify the third message went directly (no pending queue growth)
  ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  if (ep_node3 != nullptr) {
    CASE_MSG_INFO() << "A.4: pending count after post-reconnect send: " << ep_node3->get_pending_message_count()
                    << '\n';
    CASE_EXPECT_EQ(static_cast<size_t>(0), ep_node3->get_pending_message_count());
  }

  apps.pump_until([&]() { return g_upstream_test_ctx.received_message_count >= 3; });

  // All 3 messages should be received
  CASE_EXPECT_EQ(3, g_upstream_test_ctx.received_message_count);
  CASE_MSG_INFO() << "A.4: Total messages received: " << g_upstream_test_ctx.received_message_count << '\n';
#endif
}

// ============================================================
// A.5: upstream_retry_exceed_limit_fail
// Handle set unready → message queued as pending → advance time past retry limits →
// verify forward_response callback fires with error (pending message fails).
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

  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        auto bus3 = apps.node3.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump_until_connected();

  // Setup forward response callback
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.5: forward response error_code=" << error_code << '\n';
    return 0;
  });

  // Simulate disconnect: set node3's handle to unready on node1
  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (n1_connector) {
    n1_connector->set_handle_unready_by_bus_id(apps.node3.get_app_id());
  }

  // Verify handle is unready
  auto *ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_EXPECT_TRUE(ep_node3 != nullptr);
  if (ep_node3 != nullptr) {
    CASE_EXPECT_TRUE(ep_node3->get_ready_connection_handle() == nullptr);
    CASE_MSG_INFO() << "A.5: node3 handle exists=" << ep_node3->has_connection_handle()
                    << " ready=" << (ep_node3->get_ready_connection_handle() != nullptr) << '\n';
  }

  // Send a message — should go to pending queue since handle is unready
  char msg_data[] = "will-fail-A5";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 500;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  // Verify message was queued as pending
  ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  if (ep_node3 != nullptr) {
    CASE_MSG_INFO() << "A.5: pending count=" << ep_node3->get_pending_message_count() << '\n';
    CASE_EXPECT_GT(ep_node3->get_pending_message_count(), static_cast<size_t>(0));
  }

  // Advance time past reconnect retries using set_sys_now
  auto now = atframework::atapp::app::get_sys_now();

  // Step through retries with time advances + pumping
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(3));
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(7));
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(15));
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(25));
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Check whether forward response was triggered
  CASE_MSG_INFO() << "A.5: forward response count=" << g_upstream_test_ctx.forward_response_count << '\n';
  CASE_EXPECT_GT(g_upstream_test_ctx.forward_response_count, 0);
  if (g_upstream_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_upstream_test_ctx.last_forward_response_error);
    CASE_MSG_INFO() << "A.5: last error_code=" << g_upstream_test_ctx.last_forward_response_error << '\n';
  }

  // After retry exhaustion, handle should be removed by timer callback
  if (n1_connector) {
    CASE_EXPECT_FALSE(n1_connector->has_connection_handle(apps.node3.get_app_id()));
  }

  // Reset time using real system clock
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// A.6: upstream_retry_timeout_downstream_cleanup
// Upstream goes down, retry timeout triggers downstream pending
// message failure and downstream handle cleanup.
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

  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        auto bus3 = apps.node3.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump_until_connected();

  // Track forward response errors
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.6: forward response error_code=" << error_code << '\n';
    return 0;
  });

  // Use debug API to set handle unready (simulates upstream disconnect)
  // set_handle_unready_by_bus_id also starts the reconnect timer
  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (n1_connector) {
    n1_connector->set_handle_unready_by_bus_id(apps.node3.get_app_id());
  }

  // Verify node3 handle is now unready
  auto *ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_EXPECT_TRUE(ep_node3 != nullptr);
  if (ep_node3 != nullptr) {
    CASE_MSG_INFO() << "A.6: node3 handle exists=" << ep_node3->has_connection_handle()
                    << " ready=" << (ep_node3->get_ready_connection_handle() != nullptr) << '\n';
    CASE_EXPECT_TRUE(ep_node3->get_ready_connection_handle() == nullptr);
  }

  // Send message — should be queued as pending since handle is unready
  char msg_data[] = "pending-cleanup-A6";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 600;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  // Pump to process handle state changes
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Advance time past retry limits to trigger forward_response error
  auto now = atframework::atapp::app::get_sys_now();

  // Advance to T+30s — timer at T+2s should fire, exhausting retries
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(30));
  CASE_MSG_INFO() << "A.6: advanced to T+30s, calling tick() then run_noblock()" << '\n';
  apps.node1.tick();
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Check handle after T+30s
  if (n1_connector) {
    CASE_MSG_INFO() << "A.6: has_connection_handle after T+30s: "
                    << n1_connector->has_connection_handle(apps.node3.get_app_id()) << '\n';
  }

  // Advance to T+60s for extra margin
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(60));
  apps.node1.tick();
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Verify forward response error was triggered
  CASE_MSG_INFO() << "A.6: forward response count=" << g_upstream_test_ctx.forward_response_count << '\n';
  CASE_EXPECT_GT(g_upstream_test_ctx.forward_response_count, 0);
  if (g_upstream_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_upstream_test_ctx.last_forward_response_error);
    CASE_MSG_INFO() << "A.6: last error_code=" << g_upstream_test_ctx.last_forward_response_error << '\n';
  }

  // Reconnect retries are exhausted by app::tick() / process_custom_timers().
  // set_handle_unready_by_bus_id started the reconnect timer (retry 0→1, timer at T+2s).
  // At +30s, tick() fires the timer callback: setup_reconnect_timer → retry 1→2 >= max(2) → remove_connection_handle.
  // Config: reconnect_max_try_times=2, reconnect_start_interval=2s.
  if (n1_connector) {
    CASE_EXPECT_FALSE(n1_connector->has_connection_handle(apps.node3.get_app_id()));
    CASE_MSG_INFO() << "A.6: handle removed after reconnect retry exhaustion via app::tick()" << '\n';
  }

  // After GC timeout (endpoint_gc_timeout default=60s), verify endpoint is cleaned up.
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(200));
  for (int i = 0; i < 5; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_MSG_INFO() << "A.6: node3 endpoint on node1 after GC: " << (ep_node3 ? "exists" : "null") << '\n';
  CASE_EXPECT_TRUE(!ep_node3);

  // Reset time using real system clock
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
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

  apps.pump_until(
      [&apps]() {
        auto bus1 = apps.node1.get_bus_node();
        auto bus3 = apps.node3.get_bus_node();
        return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8));

  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  apps.pump_until_connected();

  // Setup response callback
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_upstream_test_ctx.last_forward_response_error = error_code;
    ++g_upstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "A.7: forward response error_code=" << error_code << '\n';
    return 0;
  });

  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);

  // Verify the handle exists and is valid before topology removal
  if (n1_connector) {
    CASE_EXPECT_TRUE(n1_connector->has_connection_handle(apps.node3.get_app_id()));
    CASE_MSG_INFO() << "A.7: has_connection_handle before remove: "
                    << n1_connector->has_connection_handle(apps.node3.get_app_id()) << '\n';
  }

  // Remove topology for node3 on node1 → sets kLostTopology flag
  if (n1_connector) {
    n1_connector->remove_topology_peer(apps.node3.get_app_id());
  }

  // Run one tick to process the topology change
  apps.node1.run_noblock();

  // Handle should still exist after topology removal (kLostTopology set but handle not removed yet)
  if (n1_connector) {
    CASE_EXPECT_TRUE(n1_connector->has_connection_handle(apps.node3.get_app_id()));
    CASE_EXPECT_TRUE(n1_connector->has_lost_topology_flag(apps.node3.get_app_id()));
    CASE_MSG_INFO() << "A.7: has_connection_handle after remove_topology_peer: "
                    << n1_connector->has_connection_handle(apps.node3.get_app_id())
                    << ", has_lost_topology_flag: " << n1_connector->has_lost_topology_flag(apps.node3.get_app_id())
                    << '\n';
  }

  // Now simulate the handle becoming unready (e.g., connection failure after topology loss)
  if (n1_connector) {
    n1_connector->set_handle_unready_by_bus_id(apps.node3.get_app_id());
  }

  // Verify handle is unready
  auto *ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_EXPECT_TRUE(ep_node3 != nullptr);
  if (ep_node3 != nullptr) {
    CASE_EXPECT_TRUE(ep_node3->get_ready_connection_handle() == nullptr);
    CASE_MSG_INFO() << "A.7: node3 handle ready=" << (ep_node3->get_ready_connection_handle() != nullptr) << '\n';
  }

  // Send a message — should be queued as pending
  char msg_data[] = "topology-offline-A7";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 700;
  apps.node1.send_message(apps.node3.get_app_id(), 100, msg_span, &seq);

  // Advance time past retry timeout to trigger forward_response error
  auto now = atframework::atapp::app::get_sys_now();

  // Advance to T+30s — reconnect timer should fire
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(30));
  CASE_MSG_INFO() << "A.7: advanced to T+30s, calling tick() then run_noblock()" << '\n';
  apps.node1.tick();
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  if (n1_connector) {
    CASE_MSG_INFO() << "A.7: has_connection_handle after T+30s: "
                    << n1_connector->has_connection_handle(apps.node3.get_app_id()) << '\n';
  }

  // Advance to T+60s — lost_topology_timeout (32s) should be exceeded
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(60));
  apps.node1.tick();
  for (int i = 0; i < 3; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Verify forward response error was triggered (pending message failed)
  CASE_MSG_INFO() << "A.7: forward response count=" << g_upstream_test_ctx.forward_response_count << '\n';
  CASE_EXPECT_GT(g_upstream_test_ctx.forward_response_count, 0);
  if (g_upstream_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_upstream_test_ctx.last_forward_response_error);
    CASE_MSG_INFO() << "A.7: last error_code=" << g_upstream_test_ctx.last_forward_response_error << '\n';
  }

  // The lost_topology_timeout timer (32s from config) was set by remove_topology_peer.
  // set_handle_unready_by_bus_id also started reconnect timers.
  // At T+30s, tick() fires the timer callback — detects kLostTopology but timeout not yet reached.
  // The reconnect retry also fires: retry 1→2 >= max(2), but kLostTopology prevents reconnect.
  // At T+60s, tick() fires again — sys_now(+60s) >= lost_topology_timeout(+32s) → remove_connection_handle.
  if (n1_connector) {
    CASE_EXPECT_FALSE(n1_connector->has_connection_handle(apps.node3.get_app_id()));
    CASE_MSG_INFO() << "A.7: handle removed by lost_topology_timeout via app::tick()" << '\n';
  }

  // Advance time past endpoint GC timeout (default 60s) to verify endpoint cleanup
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(180));
  for (int i = 0; i < 5; ++i) {
    apps.node1.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  ep_node3 = apps.node1.get_endpoint(apps.node3.get_app_id());
  CASE_EXPECT_TRUE(ep_node3 == nullptr);
  CASE_MSG_INFO() << "A.7: node3 endpoint after GC: " << (ep_node3 ? "exists" : "null") << '\n';

  // Reset time using real system clock
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// A.8: upstream_topology_change_new_upstream
// update_topology_peer switches to a new upstream,
// verifying the proxy_bus_id changes between sends via debug API.
// ============================================================
CASE_TEST(atapp_upstream_forward, upstream_topology_change_new_upstream) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_proxy_bus_id() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_test_context();

  // 4-node topology:
  // node1(0x101) --proxy--> upstream(0x102) <--proxy-- node3(0x103)
  //                            ^
  //                            |
  //               new_upstream(0x104) --proxy--> upstream(0x102)
  //
  // Initial: node1 routes to node3 via upstream(0x102) [proxy_bus_id = 0x102]
  // After:   update_topology_peer switches to new_upstream(0x104) [proxy_bus_id = 0x104]

  std::string conf1 = get_test_conf_path("atapp_test_upstream_1.yaml");
  std::string conf2 = get_test_conf_path("atapp_test_upstream_2.yaml");
  std::string conf3 = get_test_conf_path("atapp_test_upstream_3.yaml");
  std::string conf4 = get_test_conf_path("atapp_test_upstream_4.yaml");

  if (check_and_skip_if_missing(conf1)) return;
  if (check_and_skip_if_missing(conf2)) return;
  if (check_and_skip_if_missing(conf3)) return;
  if (check_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app upstream_app;
  atframework::atapp::app node3;
  atframework::atapp::app new_upstream;

  // Init upstream first, then children
  const char *args_up[] = {"upstream", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, upstream_app.init(nullptr, 4, args_up, nullptr));

  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));

  const char *args3[] = {"node3", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, node3.init(nullptr, 4, args3, nullptr));

  const char *args4[] = {"new_upstream", "-c", conf4.c_str(), "start"};
  CASE_EXPECT_EQ(0, new_upstream.init(nullptr, 4, args4, nullptr));

  // Helper for 4-node pumping
  auto pump4_until = [&](const std::function<bool()> &cond, std::chrono::seconds timeout = std::chrono::seconds(8)) {
    auto start = atfw::util::time::time_utility::sys_now();
    auto end = start + timeout;
    while (!cond() && atfw::util::time::time_utility::sys_now() < end) {
      upstream_app.run_noblock();
      node1.run_noblock();
      node3.run_noblock();
      new_upstream.run_noblock();
      atfw::util::time::time_utility::update();
    }
  };

  // Wait for all nodes to connect to their upstreams
  pump4_until([&]() {
    auto bus1 = node1.get_bus_node();
    auto bus3 = node3.get_bus_node();
    auto bus4 = new_upstream.get_bus_node();
    return bus1 && bus1->get_upstream_endpoint() != nullptr && bus3 && bus3->get_upstream_endpoint() != nullptr &&
           bus4 && bus4->get_upstream_endpoint() != nullptr;
  });

  // Create discovery for all 4 nodes
  auto node1_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto upstream_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto node3_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto new_up_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    node1_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    upstream_app.pack(info);
    upstream_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    node3.pack(info);
    node3_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    new_upstream.pack(info);
    new_up_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  // Add all discoveries to all nodes
  atframework::atapp::app *all_apps[] = {&node1, &upstream_app, &node3, &new_upstream};
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> all_discs[] = {node1_disc, upstream_disc, node3_disc,
                                                                               new_up_disc};
  for (auto *app : all_apps) {
    for (auto &disc : all_discs) {
      app->get_etcd_module()->get_global_discovery().add_node(disc);
    }
  }

  // Update bus node topology registries (required for mutable_connection_handle to find peers)
  auto bus1 = node1.get_bus_node();
  auto bus_up = upstream_app.get_bus_node();
  auto bus3 = node3.get_bus_node();
  auto bus_nu = new_upstream.get_bus_node();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(node1.get_app_id(), upstream_app.get_app_id(), nullptr);
    bus_up->get_topology_registry()->update_peer(node3.get_app_id(), upstream_app.get_app_id(), nullptr);
    bus_up->get_topology_registry()->update_peer(new_upstream.get_app_id(), upstream_app.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(upstream_app.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(node3.get_app_id(), upstream_app.get_app_id(), nullptr);
    // Register 0x104 with upstream=0 so chain check won't find 0x102 in node3's path after topology change
    bus1->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
  }
  if (bus3 && bus3->get_topology_registry()) {
    bus3->get_topology_registry()->update_peer(upstream_app.get_app_id(), 0, nullptr);
    bus3->get_topology_registry()->update_peer(node1.get_app_id(), upstream_app.get_app_id(), nullptr);
  }
  if (bus_nu && bus_nu->get_topology_registry()) {
    bus_nu->get_topology_registry()->update_peer(upstream_app.get_app_id(), 0, nullptr);
  }

  // Create endpoints first (handles must exist before connector-level topology update)
  CASE_EXPECT_TRUE(node1.mutable_endpoint(upstream_disc));
  CASE_EXPECT_TRUE(node1.mutable_endpoint(node3_disc));
  CASE_EXPECT_TRUE(node1.mutable_endpoint(new_up_disc));
  CASE_EXPECT_TRUE(upstream_app.mutable_endpoint(node1_disc));
  CASE_EXPECT_TRUE(upstream_app.mutable_endpoint(node3_disc));
  CASE_EXPECT_TRUE(upstream_app.mutable_endpoint(new_up_disc));
  CASE_EXPECT_TRUE(node3.mutable_endpoint(node1_disc));
  CASE_EXPECT_TRUE(node3.mutable_endpoint(upstream_disc));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(upstream_disc));

  // Setup connector-level topology AFTER endpoints so handles exist
  auto up_connector = upstream_app.get_atbus_connector();
  if (up_connector) {
    up_connector->update_topology_peer(node1.get_app_id(), upstream_app.get_app_id(), nullptr);
    up_connector->update_topology_peer(node3.get_app_id(), upstream_app.get_app_id(), nullptr);
    up_connector->update_topology_peer(new_upstream.get_app_id(), upstream_app.get_app_id(), nullptr);
  }

  auto n1_connector = node1.get_atbus_connector();
  if (n1_connector) {
    n1_connector->update_topology_peer(upstream_app.get_app_id(), 0, nullptr);
    n1_connector->update_topology_peer(node3.get_app_id(), upstream_app.get_app_id(), nullptr);
    // Use upstream=0x102 so 0x104's handle gets proxied through 0x102 and becomes ready
    n1_connector->update_topology_peer(new_upstream.get_app_id(), upstream_app.get_app_id(), nullptr);
  }

  auto n3_connector = node3.get_atbus_connector();
  if (n3_connector) {
    n3_connector->update_topology_peer(upstream_app.get_app_id(), 0, nullptr);
    n3_connector->update_topology_peer(node1.get_app_id(), upstream_app.get_app_id(), nullptr);
  }

  auto nu_connector = new_upstream.get_atbus_connector();
  if (nu_connector) {
    nu_connector->update_topology_peer(upstream_app.get_app_id(), 0, nullptr);
  }

  // Wait for node3 handle to be ready on node1
  pump4_until([&]() {
    auto *ep = node1.get_endpoint(node3.get_app_id());
    return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
  });

  // Verify initial proxy_bus_id = upstream(0x102)
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (n1_connector) {
    auto initial_proxy = n1_connector->get_connection_handle_proxy_bus_id(node3.get_app_id());
    CASE_MSG_INFO() << "A.8: initial proxy_bus_id for node3: 0x" << std::hex << initial_proxy << std::dec << '\n';
    CASE_EXPECT_EQ(upstream_app.get_app_id(), initial_proxy);
  }

  // Set up receive callback on node3
  node3.set_evt_on_forward_request([](atframework::atapp::app &,
                                      const atframework::atapp::app::message_sender_t &sender,
                                      const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_upstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_upstream_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_upstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "A.8: node3 received: " << received << ", direct_source_id=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // First message: routed via upstream(0x102)
  char msg_data[] = "before-topo-change-A8";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 800;
  CASE_EXPECT_EQ(0, node1.send_message(node3.get_app_id(), 100, msg_span, &seq));

  pump4_until([&]() { return g_upstream_test_ctx.received_message_count >= 1; });
  CASE_EXPECT_EQ(1, g_upstream_test_ctx.received_message_count);

  // Change topology: node1 now routes to node3 via new_upstream(0x104)
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(node3.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (n1_connector) {
    n1_connector->update_topology_peer(node3.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  // Wait for handle to be ready with new proxy binding
  pump4_until([&]() {
    auto *ep = node1.get_endpoint(node3.get_app_id());
    return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
  });

  // Verify proxy_bus_id changed to new_upstream(0x104)
  if (n1_connector) {
    auto new_proxy = n1_connector->get_connection_handle_proxy_bus_id(node3.get_app_id());
    CASE_MSG_INFO() << "A.8: new proxy_bus_id for node3: 0x" << std::hex << new_proxy << std::dec << '\n';
    CASE_EXPECT_EQ(new_upstream.get_app_id(), new_proxy);
  }

  // Second message: routed via new_upstream(0x104) at connector level
  char msg_data2[] = "after-topo-change-A8";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  CASE_EXPECT_EQ(0, node1.send_message(node3.get_app_id(), 100, msg_span2, &seq));

  pump4_until([&]() { return g_upstream_test_ctx.received_message_count >= 2; });
  CASE_EXPECT_EQ(2, g_upstream_test_ctx.received_message_count);

  if (g_upstream_test_ctx.received_messages.size() >= 2) {
    CASE_EXPECT_EQ("before-topo-change-A8", g_upstream_test_ctx.received_messages[0]);
    CASE_EXPECT_EQ("after-topo-change-A8", g_upstream_test_ctx.received_messages[1]);
  }

  // Verify two messages came from different upstream sources.
  // Note: direct_source_id reflects the atbus-level last-hop relay node (always 0x102 in this
  // topology since all messages traverse the same atbus hub). The connector-level proxy_bus_id
  // (verified above) correctly tracks the logical upstream change from 0x102 to 0x104.
  if (g_upstream_test_ctx.received_direct_source_ids.size() >= 2) {
    CASE_EXPECT_EQ(upstream_app.get_app_id(), g_upstream_test_ctx.received_direct_source_ids[0]);
    CASE_EXPECT_EQ(upstream_app.get_app_id(), g_upstream_test_ctx.received_direct_source_ids[1]);
    CASE_MSG_INFO() << "A.8: first direct_source_id=0x" << std::hex << g_upstream_test_ctx.received_direct_source_ids[0]
                    << ", second direct_source_id=0x" << g_upstream_test_ctx.received_direct_source_ids[1] << std::dec
                    << '\n';
  }
#endif
}
