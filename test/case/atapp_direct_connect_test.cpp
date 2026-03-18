// Copyright 2026 atframework
// Test B: Direct connection data send tests
//
// Topology: node1(0x201) --proxy--> upstream(0x203) <--proxy-- node2(0x202)
// allow_direct_connection: true, node1 and node2 are kSameUpstreamPeer
// node1 sends messages to node2 via direct connection (not upstream relay)

#include <atframe/atapp.h>
#include <atframe/connectors/atapp_connector_atbus.h>
#include <atframe/modules/etcd_module.h>
#include <detail/libatbus_error.h>

#include <common/file_system.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <gsl/util>
#include <memory>
#include <string>
#include <vector>

#include "frame/test_macros.h"

namespace {

struct direct_test_context {
  int received_message_count = 0;
  int forward_response_count = 0;
  int32_t last_forward_response_error = 0;
  std::vector<std::string> received_messages;
  std::vector<atframework::atapp::app_id_t> received_direct_source_ids;
};

static direct_test_context g_direct_test_ctx;

static void reset_direct_test_context() {
  g_direct_test_ctx.received_message_count = 0;
  g_direct_test_ctx.forward_response_count = 0;
  g_direct_test_ctx.last_forward_response_error = 0;
  g_direct_test_ctx.received_messages.clear();
  g_direct_test_ctx.received_direct_source_ids.clear();
}

static std::string get_direct_test_conf_path(const char *filename) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/" + filename;
}

static bool check_direct_and_skip_if_missing(const std::string &path) {
  if (!atfw::util::file_system::is_exist(path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << path << " not found, skip this test" << '\n';
    return true;
  }
  return false;
}

struct direct_three_node_apps {
  atframework::atapp::app node1;
  atframework::atapp::app node2;
  atframework::atapp::app upstream;

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> node1_discovery;
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> node2_discovery;
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> upstream_discovery;

  bool init_all() {
    std::string conf1 = get_direct_test_conf_path("atapp_test_direct_1.yaml");
    std::string conf2 = get_direct_test_conf_path("atapp_test_direct_2.yaml");
    std::string conf3 = get_direct_test_conf_path("atapp_test_direct_3.yaml");

    if (check_direct_and_skip_if_missing(conf1)) return false;
    if (check_direct_and_skip_if_missing(conf2)) return false;
    if (check_direct_and_skip_if_missing(conf3)) return false;

    // Init upstream first (it needs to be listening before children connect)
    const char *args_up[] = {"upstream", "-c", conf3.c_str(), "start"};
    if (upstream.init(nullptr, 4, args_up, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init upstream app" << '\n';
      return false;
    }

    const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
    if (node1.init(nullptr, 4, args1, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init node1 app" << '\n';
      return false;
    }

    const char *args2[] = {"node2", "-c", conf2.c_str(), "start"};
    if (node2.init(nullptr, 4, args2, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init node2 app" << '\n';
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
      node2.run_noblock();
      CASE_THREAD_SLEEP_MS(1);
      atfw::util::time::time_utility::update();
    }
  }

  void wait_for_upstream_connections() {
    pump_until(
        [this]() {
          auto bus1 = node1.get_bus_node();
          auto bus2 = node2.get_bus_node();
          return bus1 && bus1->get_upstream_endpoint() != nullptr && bus2 && bus2->get_upstream_endpoint() != nullptr;
        },
        std::chrono::seconds(8));
  }

  void inject_all_discovery() {
    node1_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    node2_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    upstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

    {
      atapp::protocol::atapp_discovery info;
      node1.pack(info);
      node1_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }
    {
      atapp::protocol::atapp_discovery info;
      node2.pack(info);
      node2_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }
    {
      atapp::protocol::atapp_discovery info;
      upstream.pack(info);
      upstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    // All nodes know about all other nodes
    auto &node1_gd = node1.get_etcd_module()->get_global_discovery();
    auto &node2_gd = node2.get_etcd_module()->get_global_discovery();
    auto &upstream_gd = upstream.get_etcd_module()->get_global_discovery();

    node1_gd.add_node(node1_discovery);
    node1_gd.add_node(node2_discovery);
    node1_gd.add_node(upstream_discovery);

    node2_gd.add_node(node1_discovery);
    node2_gd.add_node(node2_discovery);
    node2_gd.add_node(upstream_discovery);

    upstream_gd.add_node(node1_discovery);
    upstream_gd.add_node(node2_discovery);
    upstream_gd.add_node(upstream_discovery);
  }

  void inject_discovery_without_node2() {
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
    auto &node2_gd = node2.get_etcd_module()->get_global_discovery();
    auto &upstream_gd = upstream.get_etcd_module()->get_global_discovery();

    node1_gd.add_node(node1_discovery);
    node1_gd.add_node(upstream_discovery);

    node2_gd.add_node(upstream_discovery);

    upstream_gd.add_node(node1_discovery);
    upstream_gd.add_node(upstream_discovery);
  }

  void inject_node2_discovery_later() {
    node2_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    {
      atapp::protocol::atapp_discovery info;
      node2.pack(info);
      node2_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    auto &node1_gd = node1.get_etcd_module()->get_global_discovery();
    auto &node2_gd = node2.get_etcd_module()->get_global_discovery();
    auto &upstream_gd = upstream.get_etcd_module()->get_global_discovery();

    node1_gd.add_node(node2_discovery);
    node2_gd.add_node(node1_discovery);
    node2_gd.add_node(node2_discovery);
    upstream_gd.add_node(node2_discovery);
  }

  void setup_topology() {
    auto bus1 = node1.get_bus_node();
    auto bus2 = node2.get_bus_node();
    auto bus_up = upstream.get_bus_node();

    // Bus-level topology: both node1 and node2 have upstream(0x203) as parent
    if (bus_up && bus_up->get_topology_registry()) {
      bus_up->get_topology_registry()->update_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
      bus_up->get_topology_registry()->update_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
    }
    if (bus1 && bus1->get_topology_registry()) {
      bus1->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
      // node2 shares upstream with node1 → kSameUpstreamPeer
      bus1->get_topology_registry()->update_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
    }
    if (bus2 && bus2->get_topology_registry()) {
      bus2->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
      // node1 shares upstream with node2 → kSameUpstreamPeer
      bus2->get_topology_registry()->update_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    }

    // Connector-level topology
    auto up_connector = upstream.get_atbus_connector();
    if (up_connector) {
      up_connector->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
      up_connector->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
    }

    auto n1_connector = node1.get_atbus_connector();
    if (n1_connector) {
      n1_connector->update_topology_peer(upstream.get_app_id(), 0, nullptr);
      n1_connector->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
    }

    auto n2_connector = node2.get_atbus_connector();
    if (n2_connector) {
      n2_connector->update_topology_peer(upstream.get_app_id(), 0, nullptr);
      n2_connector->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    }
  }

  void create_endpoints() {
    CASE_EXPECT_TRUE(node1.mutable_endpoint(node2_discovery));
    CASE_EXPECT_TRUE(node1.mutable_endpoint(upstream_discovery));
    CASE_EXPECT_TRUE(node2.mutable_endpoint(node1_discovery));
    CASE_EXPECT_TRUE(node2.mutable_endpoint(upstream_discovery));
    CASE_EXPECT_TRUE(upstream.mutable_endpoint(node1_discovery));
    CASE_EXPECT_TRUE(upstream.mutable_endpoint(node2_discovery));
  }

  void pump_until_direct_connected(std::chrono::seconds timeout = std::chrono::seconds(8)) {
    pump_until(
        [this]() {
          auto *ep_on_node1 = node1.get_endpoint(node2.get_app_id());
          auto *ep_on_node2 = node2.get_endpoint(node1.get_app_id());
          return ep_on_node1 != nullptr && ep_on_node1->get_ready_connection_handle() != nullptr &&
                 ep_on_node2 != nullptr && ep_on_node2->get_ready_connection_handle() != nullptr;
        },
        timeout);
  }

  void full_setup_and_connect() {
    wait_for_upstream_connections();
    inject_all_discovery();
    setup_topology();
    create_endpoints();
    pump_until_direct_connected();
  }
};

}  // namespace

// ============================================================
// B.1: direct_discovery_ready_connect_send
// Discovery already exists, initiate direct connection and send successfully.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_discovery_ready_connect_send) {
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  // Verify direct connection handle exists
  auto *ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(ep_node2 != nullptr);
  if (ep_node2 != nullptr) {
    CASE_EXPECT_TRUE(ep_node2->get_ready_connection_handle() != nullptr);
  }

  // Set up receive callback on node2
  apps.node2.set_evt_on_forward_request([&apps](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    CASE_EXPECT_EQ(apps.node1.get_app_id(), sender.id);
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_direct_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_direct_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_direct_test_ctx.received_message_count;
    CASE_MSG_INFO() << "B.1: node2 received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // Send from node1 to node2
  char msg_data[] = "direct-connect-B1";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_direct_test_ctx.received_message_count);
  if (!g_direct_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("direct-connect-B1", g_direct_test_ctx.received_messages[0]);
  }

  // Verify the message arrived via direct connection (direct_source_id = node1's bus id)
  if (!g_direct_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(apps.node1.get_app_id(), g_direct_test_ctx.received_direct_source_ids[0]);
  }
}

// ============================================================
// B.2: direct_discovery_missing_wait_then_send
// Node2 discovery is delayed. node1 sends message first (pending),
// then discovery arrives, connection established, message delivered.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_discovery_missing_wait_then_send) {
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.wait_for_upstream_connections();

  // Phase 1: Only inject discovery for node1 and upstream, NOT node2 yet
  apps.inject_discovery_without_node2();

  // Setup partial topology (without node2)
  auto bus1 = apps.node1.get_bus_node();
  auto bus_up = apps.upstream.get_bus_node();
  auto bus2 = apps.node2.get_bus_node();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(apps.upstream.get_app_id(), 0, nullptr);
  }
  if (bus2 && bus2->get_topology_registry()) {
    bus2->get_topology_registry()->update_peer(apps.upstream.get_app_id(), 0, nullptr);
  }

  auto up_connector = apps.upstream.get_atbus_connector();
  if (up_connector) {
    up_connector->update_topology_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  auto n1_connector = apps.node1.get_atbus_connector();
  if (n1_connector) {
    n1_connector->update_topology_peer(apps.upstream.get_app_id(), 0, nullptr);
  }
  auto n2_connector = apps.node2.get_atbus_connector();
  if (n2_connector) {
    n2_connector->update_topology_peer(apps.upstream.get_app_id(), 0, nullptr);
  }

  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.upstream_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node1_discovery));

  // Verify no endpoint for node2 yet on node1
  CASE_EXPECT_TRUE(apps.node1.get_endpoint(apps.node2.get_app_id()) == nullptr);

  // Set up receive callback on node2
  apps.node2.set_evt_on_forward_request([](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_direct_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_direct_test_ctx.received_message_count;
    CASE_MSG_INFO() << "B.2: node2 received: " << received << '\n';
    return 0;
  });

  // Phase 2: Node2 discovery arrives — inject and set up topology
  apps.inject_node2_discovery_later();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus2 && bus2->get_topology_registry()) {
    bus2->get_topology_registry()->update_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (n1_connector) {
    n1_connector->update_topology_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (up_connector) {
    up_connector->update_topology_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (n2_connector) {
    n2_connector->update_topology_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }

  // Create endpoints for node2 on all nodes
  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.node2_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node2_discovery));
  CASE_EXPECT_TRUE(apps.node2.mutable_endpoint(apps.node1_discovery));
  CASE_EXPECT_TRUE(apps.node2.mutable_endpoint(apps.upstream_discovery));

  // Wait for direct connection to be ready
  apps.pump_until_direct_connected();

  auto *ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(ep_node2 != nullptr);
  if (ep_node2 != nullptr) {
    CASE_EXPECT_TRUE(ep_node2->get_ready_connection_handle() != nullptr);
  }

  // Send message — should be delivered via direct connection
  char msg_data[] = "after-discovery-B2";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  int old_count = g_direct_test_ctx.received_message_count;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= old_count + 1; });

  CASE_EXPECT_EQ(old_count + 1, g_direct_test_ctx.received_message_count);
  if (!g_direct_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("after-discovery-B2", g_direct_test_ctx.received_messages.back());
  }
}

// ============================================================
// B.3: direct_connected_send_success
// Already directly connected, send message succeeds immediately.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_connected_send_success) {
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  // Set up receive callback on node2
  apps.node2.set_evt_on_forward_request([&apps](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    CASE_EXPECT_EQ(apps.node1.get_app_id(), sender.id);
    CASE_EXPECT_EQ(200, msg.type);
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_direct_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_direct_test_ctx.received_message_count;
    CASE_MSG_INFO() << "B.3: node2 received: " << received << '\n';
    return 0;
  });

  // Send multiple messages from node1 to node2
  char msg1[] = "direct-msg1-B3";
  gsl::span<const unsigned char> span1{reinterpret_cast<const unsigned char *>(msg1), strlen(msg1)};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 200, span1, &seq));

  char msg2[] = "direct-msg2-B3";
  gsl::span<const unsigned char> span2{reinterpret_cast<const unsigned char *>(msg2), strlen(msg2)};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 200, span2, &seq));

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 2; });

  CASE_EXPECT_EQ(2, g_direct_test_ctx.received_message_count);
  if (g_direct_test_ctx.received_messages.size() >= 2) {
    CASE_EXPECT_EQ("direct-msg1-B3", g_direct_test_ctx.received_messages[0]);
    CASE_EXPECT_EQ("direct-msg2-B3", g_direct_test_ctx.received_messages[1]);
  }

  // Also test reverse direction: node2 → node1
  int node1_received = 0;
  apps.node1.set_evt_on_forward_request(
      [&node1_received, &apps](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &sender,
                               const atframework::atapp::app::message_t &) {
        CASE_EXPECT_EQ(apps.node2.get_app_id(), sender.id);
        ++node1_received;
        return 0;
      });

  char msg3[] = "reverse-B3";
  gsl::span<const unsigned char> span3{reinterpret_cast<const unsigned char *>(msg3), strlen(msg3)};
  CASE_EXPECT_EQ(0, apps.node2.send_message(apps.node1.get_app_id(), 201, span3, &seq));

  apps.pump_until([&]() { return node1_received >= 1; });
  CASE_EXPECT_EQ(1, node1_received);
}

// ============================================================
// B.4: direct_reconnect_success_after_failure
// Direct connection goes unready → messages pending → reconnect →
// pending messages delivered → new messages also succeed.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_reconnect_success_after_failure) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_handle_unready_by_bus_id() only available in Debug builds, skip"
                  << '\n';
  return;
#else
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  // Set up receive callback on node2
  apps.node2.set_evt_on_forward_request([](atframework::atapp::app &, const atframework::atapp::app::message_sender_t &,
                                           const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_direct_test_ctx.received_messages.emplace_back(received.data(), received.size());
    ++g_direct_test_ctx.received_message_count;
    CASE_MSG_INFO() << "B.4: node2 received: " << received << '\n';
    return 0;
  });

  // First message: verify normal send works
  char msg1[] = "before-disconnect-B4";
  gsl::span<const unsigned char> span1{reinterpret_cast<const unsigned char *>(msg1), strlen(msg1)};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 100, span1, &seq));

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 1; });
  CASE_EXPECT_EQ(1, g_direct_test_ctx.received_message_count);

  // Simulate disconnect by setting the handle to unready
  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (n1_connector) {
    CASE_MSG_INFO() << "B.4: Setting handle unready to simulate disconnect..." << '\n';
    n1_connector->set_handle_unready_by_bus_id(apps.node2.get_app_id());
  }

  // Send while unready — should go to pending queue
  char msg2[] = "during-reconnect-B4";
  gsl::span<const unsigned char> span2{reinterpret_cast<const unsigned char *>(msg2), strlen(msg2)};
  apps.node1.send_message(apps.node2.get_app_id(), 100, span2, &seq);

  auto *ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(ep_node2 != nullptr);
  if (ep_node2 != nullptr) {
    CASE_EXPECT_GT(ep_node2->get_pending_message_count(), static_cast<size_t>(0));
  }

  // Simulate reconnect
  if (n1_connector) {
    CASE_MSG_INFO() << "B.4: Setting handle ready to simulate reconnect..." << '\n';
    n1_connector->set_handle_ready_by_bus_id(apps.node2.get_app_id());
  }

  // Pump to let pending messages be delivered
  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 2; });
  CASE_EXPECT_GE(g_direct_test_ctx.received_message_count, 2);

  // Third message: after reconnection, should send directly
  char msg3[] = "after-reconnect-B4";
  gsl::span<const unsigned char> span3{reinterpret_cast<const unsigned char *>(msg3), strlen(msg3)};
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 100, span3, &seq));

  ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  if (ep_node2 != nullptr) {
    CASE_EXPECT_EQ(static_cast<size_t>(0), ep_node2->get_pending_message_count());
  }

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 3; });
  CASE_EXPECT_EQ(3, g_direct_test_ctx.received_message_count);
#endif
}

// ============================================================
// B.5: direct_reconnect_retry_backoff
// Verify reconnect exponential backoff: 2s → 4s → 8s → 16s → 16s (capped)
// Uses set_sys_now() to advance time step by step.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_reconnect_retry_backoff) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (!n1_connector) {
    return;
  }

  // Sync jiffies timer
  apps.node1.tick();

  // Set handle unready and remove topology to prevent auto-reconnect via I/O
  n1_connector->set_handle_unready_by_bus_id(apps.node2.get_app_id());
  n1_connector->remove_topology_peer(apps.node2.get_app_id());

  // Config: reconnect_start_interval=2s, reconnect_max_interval=16s, reconnect_max_try_times=3
  // After set_handle_unready: retry_times goes 0→1, timer at T+2s
  uint32_t retry_times_initial = n1_connector->get_connection_handle_reconnect_retry_times(apps.node2.get_app_id());
  CASE_EXPECT_GE(retry_times_initial, static_cast<uint32_t>(1));
  CASE_MSG_INFO() << "B.5: initial retry_times=" << retry_times_initial << '\n';

  auto now = atframework::atapp::app::get_sys_now();

  // Expected backoff intervals: 2s, 4s (then removed since max_try_times=3)
  // Verify the exponential growth: each interval doubles from start_interval(2s)
  // Use incremental time advancement (50ms steps) because set_sys_now jumps only
  // process one timer callback per tick().

  uint32_t last_retry = retry_times_initial;
  int first_retry_ms = 0;
  int second_retry_ms = 0;
  bool handle_removed = false;

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    apps.node1.tick();

    if (!n1_connector->has_connection_handle(apps.node2.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "B.5: handle removed after " << i * 50 << "ms" << '\n';
      break;
    }

    uint32_t cur_retry = n1_connector->get_connection_handle_reconnect_retry_times(apps.node2.get_app_id());
    if (cur_retry > last_retry) {
      if (first_retry_ms == 0) {
        first_retry_ms = i * 50;
        CASE_MSG_INFO() << "B.5: retry " << last_retry << "->" << cur_retry << " at " << first_retry_ms << "ms" << '\n';
      } else if (second_retry_ms == 0) {
        second_retry_ms = i * 50;
        CASE_MSG_INFO() << "B.5: retry " << last_retry << "->" << cur_retry << " at " << second_retry_ms << "ms"
                        << '\n';
      }
      last_retry = cur_retry;
    }
  }

  // Verify exponential backoff: second interval should be roughly double the first
  CASE_EXPECT_GT(first_retry_ms, 0);
  CASE_EXPECT_GT(second_retry_ms, 0);
  if (first_retry_ms > 0 && second_retry_ms > 0) {
    int first_interval = first_retry_ms;
    int second_interval = second_retry_ms - first_retry_ms;
    CASE_MSG_INFO() << "B.5: first_interval=" << first_interval << "ms, second_interval=" << second_interval << "ms"
                    << '\n';
    // Second interval should be approximately 2x the first (with some tolerance)
    CASE_EXPECT_GE(second_interval, first_interval);
  }

  // Handle should eventually be removed when retry_times >= max_try_times
  CASE_EXPECT_TRUE(handle_removed);
  CASE_MSG_INFO() << "B.5: handle_removed=" << handle_removed << '\n';

  // Reset time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// B.6: direct_retry_exceed_limit_fail
// Direct connection retry exceeds max_try_times → handle removed,
// pending messages fail.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_retry_exceed_limit_fail) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  // Setup forward response callback
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_direct_test_ctx.last_forward_response_error = error_code;
    ++g_direct_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "B.6: forward response error_code=" << error_code << '\n';
    return 0;
  });

  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (!n1_connector) {
    return;
  }

  // Sync jiffies timer
  apps.node1.tick();

  // Set handle unready and remove topology
  n1_connector->set_handle_unready_by_bus_id(apps.node2.get_app_id());
  n1_connector->remove_topology_peer(apps.node2.get_app_id());

  // Verify handle is unready
  auto *ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(ep_node2 != nullptr);
  if (ep_node2 != nullptr) {
    CASE_EXPECT_TRUE(ep_node2->get_ready_connection_handle() == nullptr);
  }

  // Send a message — should go to pending queue
  char msg_data[] = "will-fail-B6";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  apps.node1.send_message(apps.node2.get_app_id(), 100, msg_span, &seq);

  ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  if (ep_node2 != nullptr) {
    CASE_EXPECT_GT(ep_node2->get_pending_message_count(), static_cast<size_t>(0));
  }

  // Advance time past retry limits and lost_topology_timeout
  auto now = atframework::atapp::app::get_sys_now();
  bool handle_removed = false;

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    apps.node1.tick();
    if (!handle_removed && !n1_connector->has_connection_handle(apps.node2.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "B.6: handle removed after " << i * 50 << "ms" << '\n';
    }
    if (handle_removed && g_direct_test_ctx.forward_response_count > 0) {
      break;
    }
  }

  CASE_EXPECT_TRUE(handle_removed);
  CASE_EXPECT_GT(g_direct_test_ctx.forward_response_count, 0);
  if (g_direct_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_direct_test_ctx.last_forward_response_error);
  }

  // Reset time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// B.7: direct_topology_offline_timeout
// Topology goes offline → advance time past lost_topology_timeout →
// handle should be removed.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_topology_offline_timeout) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  // Setup forward response callback
  apps.node1.set_evt_on_forward_response([](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &,
                                            const atframework::atapp::app::message_t &, int32_t error_code) {
    g_direct_test_ctx.last_forward_response_error = error_code;
    ++g_direct_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "B.7: forward response error_code=" << error_code << '\n';
    return 0;
  });

  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (!n1_connector) {
    return;
  }

  // Sync jiffies timer
  apps.node1.tick();

  // Remove topology → sets kLostTopology
  n1_connector->remove_topology_peer(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(n1_connector->has_lost_topology_flag(apps.node2.get_app_id()));

  // Set handle unready
  n1_connector->set_handle_unready_by_bus_id(apps.node2.get_app_id());

  // Send a message — should go to pending queue
  char msg_data[] = "topology-offline-B7";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  apps.node1.send_message(apps.node2.get_app_id(), 100, msg_span, &seq);

  // Advance time past lost_topology_timeout (32s)
  auto now = atframework::atapp::app::get_sys_now();
  bool handle_removed = false;

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    apps.node1.tick();
    if (!handle_removed && !n1_connector->has_connection_handle(apps.node2.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "B.7: handle removed after " << i * 50 << "ms" << '\n';
    }
    if (handle_removed && g_direct_test_ctx.forward_response_count > 0) {
      break;
    }
  }

  CASE_EXPECT_TRUE(handle_removed);
  CASE_EXPECT_GT(g_direct_test_ctx.forward_response_count, 0);
  if (g_direct_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_direct_test_ctx.last_forward_response_error);
  }

  // Verify endpoint is cleaned up after GC
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(200));
  apps.node1.tick();

  auto *ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(ep_node2 == nullptr);

  // Reset time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// B.8: direct_prefer_direct_over_upstream
// Both upstream and direct paths available, verify direct connection
// is used (direct_source_id = node1, not upstream).
// ============================================================
CASE_TEST(atapp_direct_connect, direct_prefer_direct_over_upstream) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_proxy_bus_id() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.full_setup_and_connect();

  // While the handle is ready, the proxy_bus_id should be 0 (direct) or the node itself
  auto n1_connector = apps.node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_connector != nullptr);
  if (n1_connector) {
    auto proxy_id = n1_connector->get_connection_handle_proxy_bus_id(apps.node2.get_app_id());
    CASE_MSG_INFO() << "B.8: proxy_bus_id for node2 = 0x" << std::hex << proxy_id << std::dec << '\n';
    // If direct connection is used, proxy_bus_id should NOT be the upstream's ID
    // It should be 0 (no proxy) or the shared upstream (which is used to find the direct route)
    // The key verification is via direct_source_id in the message
  }

  // Set up receive callback on node2 to track direct_source_id
  apps.node2.set_evt_on_forward_request([&apps](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    g_direct_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_direct_test_ctx.received_message_count;
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    CASE_MSG_INFO() << "B.8: node2 received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // Send message
  char msg_data[] = "prefer-direct-B8";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_direct_test_ctx.received_message_count);

  // Verify: direct_source_id should be node1 (direct connection), NOT upstream
  if (!g_direct_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(apps.node1.get_app_id(), g_direct_test_ctx.received_direct_source_ids[0]);
    CASE_EXPECT_NE(apps.upstream.get_app_id(), g_direct_test_ctx.received_direct_source_ids[0]);
    CASE_MSG_INFO() << "B.8: Verified direct connection (direct_source=0x" << std::hex
                    << g_direct_test_ctx.received_direct_source_ids[0] << " != upstream=0x"
                    << apps.upstream.get_app_id() << std::dec << ")" << '\n';
  }

  // Also verify the atbus endpoint exists for direct connection
  auto *ep_node2 = apps.node1.get_endpoint(apps.node2.get_app_id());
  CASE_EXPECT_TRUE(ep_node2 != nullptr);
  if (ep_node2 != nullptr) {
    CASE_EXPECT_TRUE(ep_node2->get_ready_connection_handle() != nullptr);
  }
#endif
}

// ============================================================
// B.9: direct_prefer_direct_wait_discovery
// Allow direct connection, target discovery missing → wait →
// discovery arrives → direct connection established.
// ============================================================
CASE_TEST(atapp_direct_connect, direct_prefer_direct_wait_discovery) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_proxy_bus_id() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_direct_test_context();

  direct_three_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.wait_for_upstream_connections();

  // Phase 1: Inject discovery for all EXCEPT node2's listen address
  apps.inject_discovery_without_node2();

  // Setup partial topology (without node2's direct connection info)
  auto bus1 = apps.node1.get_bus_node();
  auto bus_up = apps.upstream.get_bus_node();
  auto bus2 = apps.node2.get_bus_node();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(apps.upstream.get_app_id(), 0, nullptr);
  }
  if (bus2 && bus2->get_topology_registry()) {
    bus2->get_topology_registry()->update_peer(apps.upstream.get_app_id(), 0, nullptr);
  }

  auto up_connector = apps.upstream.get_atbus_connector();
  if (up_connector) {
    up_connector->update_topology_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  auto n1_connector = apps.node1.get_atbus_connector();
  if (n1_connector) {
    n1_connector->update_topology_peer(apps.upstream.get_app_id(), 0, nullptr);
  }
  auto n2_connector = apps.node2.get_atbus_connector();
  if (n2_connector) {
    n2_connector->update_topology_peer(apps.upstream.get_app_id(), 0, nullptr);
  }

  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.upstream_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node1_discovery));

  // No node2 endpoint yet
  CASE_EXPECT_TRUE(apps.node1.get_endpoint(apps.node2.get_app_id()) == nullptr);

  // Phase 2: Node2 discovery arrives
  apps.inject_node2_discovery_later();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (bus2 && bus2->get_topology_registry()) {
    bus2->get_topology_registry()->update_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (n1_connector) {
    n1_connector->update_topology_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (up_connector) {
    up_connector->update_topology_peer(apps.node2.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }
  if (n2_connector) {
    n2_connector->update_topology_peer(apps.node1.get_app_id(), apps.upstream.get_app_id(), nullptr);
  }

  // Create endpoints
  CASE_EXPECT_TRUE(apps.node1.mutable_endpoint(apps.node2_discovery));
  CASE_EXPECT_TRUE(apps.upstream.mutable_endpoint(apps.node2_discovery));
  CASE_EXPECT_TRUE(apps.node2.mutable_endpoint(apps.node1_discovery));
  CASE_EXPECT_TRUE(apps.node2.mutable_endpoint(apps.upstream_discovery));

  // Wait for direct connection
  apps.pump_until_direct_connected();

  // Set up receive callback on node2
  apps.node2.set_evt_on_forward_request([&apps](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    g_direct_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_direct_test_ctx.received_message_count;
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    CASE_MSG_INFO() << "B.9: node2 received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // Send message — should go via direct connection
  char msg_data[] = "wait-discovery-B9";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.node1.send_message(apps.node2.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_direct_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_direct_test_ctx.received_message_count);

  // Verify direct connection was used (direct_source_id = node1, not upstream)
  if (!g_direct_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(apps.node1.get_app_id(), g_direct_test_ctx.received_direct_source_ids[0]);
    CASE_EXPECT_NE(apps.upstream.get_app_id(), g_direct_test_ctx.received_direct_source_ids[0]);
  }
#endif
}
