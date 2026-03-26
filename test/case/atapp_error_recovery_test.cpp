// Copyright 2026 atframework
// Test F: Error recovery & proxy cascade tests
//
// Topology: node1(0x601) --proxy--> proxy(0x602) <--proxy-- downstream(0x603)
// allow_direct_connection: false
//
// F.1: send returns NO_CONNECTION → handle unready → bus reconnects → send success
// F.2: proxy ready cascade → downstream ready + pending messages flushed
// F.3: proxy unready cascade → downstream also unready
// F.4: proxy handle removed → downstream on_close_connection → both removed
// F.5: downstream removed first → proxy removed → stale proxy_for handled safely

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

#if !defined(NDEBUG)

// Mirror of atbus_connection_handle_flags_t from atapp_connector_atbus.cpp (internal enum)
static constexpr uint32_t kAtbusHandleFlagReady = 0x08;

struct recovery_test_context {
  int received_message_count = 0;
  std::vector<std::string> received_messages;
  std::vector<atframework::atapp::app_id_t> received_direct_source_ids;
};

static recovery_test_context g_recovery_test_ctx;

static void reset_recovery_test_context() {
  g_recovery_test_ctx.received_message_count = 0;
  g_recovery_test_ctx.received_messages.clear();
  g_recovery_test_ctx.received_direct_source_ids.clear();
}

static std::string get_recovery_test_conf_path(const char *filename) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/" + filename;
}

static bool check_recovery_and_skip_if_missing(const std::string &path) {
  if (!atfw::util::file_system::is_exist(path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << path << " not found, skip this test" << '\n';
    return true;
  }
  return false;
}

template <typename... Apps>
static void pump_apps_until(const std::function<bool()> &cond, std::chrono::seconds timeout, Apps &...apps) {
  auto start = atfw::util::time::time_utility::sys_now();
  auto end = start + timeout;
  while (!cond() && atfw::util::time::time_utility::sys_now() < end) {
    int dummy[] = {(apps.run_noblock(), 0)...};
    (void)dummy;
    CASE_THREAD_SLEEP_MS(1);
    atfw::util::time::time_utility::update();
  }
}

static void setup_receive_callback(atframework::atapp::app &target, const char *label) {
  target.set_evt_on_forward_request([label](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &sender,
                                            const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_recovery_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_recovery_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_recovery_test_ctx.received_message_count;
    CASE_MSG_INFO() << label << ": received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });
}

static int send_test_message(atframework::atapp::app &from, atframework::atapp::app_id_t to, const char *data) {
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(data),
                                          static_cast<size_t>(strlen(data))};
  uint64_t seq = 0;
  return from.send_message(to, 100, msg_span, &seq);
}

// Setup 3-node topology and discovery for F group
// Topology: node1 --upstream--> proxy <--upstream-- downstream
static void setup_recovery_test_env(atframework::atapp::app &node1, atframework::atapp::app &proxy_node,
                                    atframework::atapp::app &downstream,
                                    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &n1_disc,
                                    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &proxy_disc,
                                    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &down_disc) {
  auto bus1 = node1.get_bus_node();
  auto bus_proxy = proxy_node.get_bus_node();
  auto bus_down = downstream.get_bus_node();

  // Update bus topology registries
  if (bus_proxy && bus_proxy->get_topology_registry()) {
    bus_proxy->get_topology_registry()->update_peer(node1.get_app_id(), proxy_node.get_app_id(), nullptr);
    bus_proxy->get_topology_registry()->update_peer(downstream.get_app_id(), proxy_node.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(proxy_node.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(downstream.get_app_id(), proxy_node.get_app_id(), nullptr);
  }
  if (bus_down && bus_down->get_topology_registry()) {
    bus_down->get_topology_registry()->update_peer(proxy_node.get_app_id(), 0, nullptr);
    bus_down->get_topology_registry()->update_peer(node1.get_app_id(), proxy_node.get_app_id(), nullptr);
  }

  // Update connector topology
  auto proxy_conn = proxy_node.get_atbus_connector();
  if (proxy_conn) {
    proxy_conn->update_topology_peer(node1.get_app_id(), proxy_node.get_app_id(), nullptr);
    proxy_conn->update_topology_peer(downstream.get_app_id(), proxy_node.get_app_id(), nullptr);
  }
  auto n1_conn = node1.get_atbus_connector();
  if (n1_conn) {
    n1_conn->update_topology_peer(proxy_node.get_app_id(), 0, nullptr);
    n1_conn->update_topology_peer(downstream.get_app_id(), proxy_node.get_app_id(), nullptr);
  }
  auto down_conn = downstream.get_atbus_connector();
  if (down_conn) {
    down_conn->update_topology_peer(proxy_node.get_app_id(), 0, nullptr);
    down_conn->update_topology_peer(node1.get_app_id(), proxy_node.get_app_id(), nullptr);
  }

  // Create discovery nodes
  n1_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  proxy_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  down_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    n1_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    proxy_node.pack(info);
    proxy_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    downstream.pack(info);
    down_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  // Add discovery to all apps
  atframework::atapp::app *apps[] = {&node1, &proxy_node, &downstream};
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> discs[] = {n1_disc, proxy_disc, down_disc};
  for (auto *app : apps) {
    for (auto &disc : discs) {
      app->get_etcd_module()->get_global_discovery().add_node(disc);
    }
  }

  // Create endpoints for all pairs
  CASE_EXPECT_TRUE(node1.mutable_endpoint(proxy_disc));
  CASE_EXPECT_TRUE(node1.mutable_endpoint(down_disc));
  CASE_EXPECT_TRUE(proxy_node.mutable_endpoint(n1_disc));
  CASE_EXPECT_TRUE(proxy_node.mutable_endpoint(down_disc));
  CASE_EXPECT_TRUE(downstream.mutable_endpoint(n1_disc));
  CASE_EXPECT_TRUE(downstream.mutable_endpoint(proxy_disc));
}

#endif

}  // namespace

// ============================================================
// F.1: send_fail_reconnect_then_success
// Handle marked unready (simulates on_send_forward_request NO_CONNECTION) →
// message queued as pending → reconnect timer fires → try_direct_reconnect
// finds bus endpoint available → set_handle_ready → pending message flushed.
// Uses E-group 3-node peer topology (allow_direct_connection: true) so that
// try_direct_reconnect proceeds for kSameUpstreamPeer (not early-returned).
// ============================================================
CASE_TEST(atapp_error_recovery, send_fail_reconnect_then_success) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs required, skip" << '\n';
  return;
#else
  reset_recovery_test_context();

  // Reuse E-group configs: node1(0x501) → upstream(0x503) ← node2(0x502)
  // allow_direct_connection: true → kSameUpstreamPeer direct connection
  std::string conf1 = get_recovery_test_conf_path("atapp_test_discovery_1.yaml");
  std::string conf2 = get_recovery_test_conf_path("atapp_test_discovery_2.yaml");
  std::string conf3 = get_recovery_test_conf_path("atapp_test_discovery_3.yaml");

  if (check_recovery_and_skip_if_missing(conf1)) return;
  if (check_recovery_and_skip_if_missing(conf2)) return;
  if (check_recovery_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app node2;
  atframework::atapp::app upstream;

  const char *args3[] = {"upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, upstream.init(nullptr, 4, args3, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args2[] = {"node2", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, node2.init(nullptr, 4, args2, nullptr));

  // Wait for bus connections
  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b2 = node2.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b2 && b2->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  // Set up discovery, topology and endpoints (reuse E group setup pattern)
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, n2_disc, up_disc;
  n1_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  n2_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  up_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    n1_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    node2.pack(info);
    n2_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    upstream.pack(info);
    up_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  // Add discovery to all apps
  atframework::atapp::app *apps[] = {&node1, &node2, &upstream};
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> discs[] = {n1_disc, n2_disc, up_disc};
  for (auto *app : apps) {
    for (auto &disc : discs) {
      app->get_etcd_module()->get_global_discovery().add_node(disc);
    }
  }

  // Set up bus topology registries
  auto bus1 = node1.get_bus_node();
  auto bus2 = node2.get_bus_node();
  auto bus_up = upstream.get_bus_node();
  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    bus_up->get_topology_registry()->update_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
  }
  if (bus2 && bus2->get_topology_registry()) {
    bus2->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
    bus2->get_topology_registry()->update_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
  }

  // Set up connector topology
  auto up_conn = upstream.get_atbus_connector();
  if (up_conn) {
    up_conn->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    up_conn->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
  }
  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;
  n1_conn->update_topology_peer(upstream.get_app_id(), 0, nullptr);
  n1_conn->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
  auto n2_conn = node2.get_atbus_connector();
  if (n2_conn) {
    n2_conn->update_topology_peer(upstream.get_app_id(), 0, nullptr);
    n2_conn->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
  }

  // Create endpoints
  CASE_EXPECT_TRUE(node1.mutable_endpoint(n2_disc));
  CASE_EXPECT_TRUE(node1.mutable_endpoint(up_disc));
  CASE_EXPECT_TRUE(node2.mutable_endpoint(n1_disc));
  CASE_EXPECT_TRUE(node2.mutable_endpoint(up_disc));
  CASE_EXPECT_TRUE(upstream.mutable_endpoint(n1_disc));
  CASE_EXPECT_TRUE(upstream.mutable_endpoint(n2_disc));

  // Pump until node1→node2 handle is ready (direct connection)
  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  // Step 1: Verify initial message delivery (direct connection)
  setup_receive_callback(node2, "F.1");
  CASE_EXPECT_EQ(0, send_test_message(node1, node2.get_app_id(), "before-fail-F1"));
  pump_apps_until([&]() { return g_recovery_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), upstream,
                  node1, node2);
  CASE_EXPECT_EQ(1, g_recovery_test_ctx.received_message_count);
  if (!g_recovery_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("before-fail-F1", g_recovery_test_ctx.received_messages[0]);
  }

  // Step 2: Force handle unready (simulates on_send_forward_request detecting NO_CONNECTION)
  // set_handle_unready_by_bus_id also starts a reconnect timer at now+start_interval (2s)
  n1_conn->set_handle_unready_by_bus_id(node2.get_app_id());
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(node2.get_app_id()));
  node1.tick();

  // Step 3: Send while unready → message goes to pending queue
  reset_recovery_test_context();
  setup_receive_callback(node2, "F.1");
  int ret = send_test_message(node1, node2.get_app_id(), "pending-F1");
  CASE_MSG_INFO() << "F.1: send while unready returned " << ret << '\n';

  // Verify message is pending (not yet delivered)
  CASE_EXPECT_EQ(0, g_recovery_test_ctx.received_message_count);
  auto *ep_node2 = node1.get_endpoint(node2.get_app_id());
  if (ep_node2 != nullptr) {
    CASE_EXPECT_GT(ep_node2->get_pending_message_count(), static_cast<size_t>(0));
    CASE_MSG_INFO() << "F.1: pending count after send=" << ep_node2->get_pending_message_count() << '\n';
  }

  // Step 4: Pump until reconnect timer fires (2s) → try_direct_reconnect finds
  // bus endpoint still available (kSameUpstreamPeer, proxy_bus_id=0) →
  // on_start_connect_to_connected_endpoint → set_handle_ready → pending flushed
  pump_apps_until([&]() { return n1_conn->is_connection_handle_ready(node2.get_app_id()); }, std::chrono::seconds(8),
                  upstream, node1, node2);
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  // Step 5: Pump to let pending messages be delivered
  pump_apps_until([&]() { return g_recovery_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), upstream,
                  node1, node2);
  CASE_EXPECT_EQ(1, g_recovery_test_ctx.received_message_count);
  if (!g_recovery_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("pending-F1", g_recovery_test_ctx.received_messages[0]);
  }
#endif
}

// ============================================================
// F.2: proxy_ready_cascade_downstream
// proxy unready → downstream unready → send pending →
// proxy ready → cascade → downstream ready → pending flushed.
// ============================================================
CASE_TEST(atapp_error_recovery, proxy_ready_cascade_downstream) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs required, skip" << '\n';
  return;
#else
  reset_recovery_test_context();

  std::string conf1 = get_recovery_test_conf_path("atapp_test_recovery_1.yaml");
  std::string conf2 = get_recovery_test_conf_path("atapp_test_recovery_2.yaml");
  std::string conf3 = get_recovery_test_conf_path("atapp_test_recovery_3.yaml");

  if (check_recovery_and_skip_if_missing(conf1)) return;
  if (check_recovery_and_skip_if_missing(conf2)) return;
  if (check_recovery_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app proxy_node;
  atframework::atapp::app downstream;

  const char *args2[] = {"proxy", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, proxy_node.init(nullptr, 4, args2, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args3[] = {"downstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, downstream.init(nullptr, 4, args3, nullptr));

  // Wait for bus connections
  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b3 = downstream.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b3 && b3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), proxy_node, node1, downstream);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, proxy_disc, down_disc;
  setup_recovery_test_env(node1, proxy_node, downstream, n1_disc, proxy_disc, down_disc);

  // Pump until node1→downstream handle is ready (routed through proxy)
  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  pump_apps_until([&]() { return n1_conn->is_connection_handle_ready(downstream.get_app_id()); },
                  std::chrono::seconds(8), proxy_node, node1, downstream);
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(downstream.get_app_id()));

  // Verify proxy_for relationship: proxy handle should proxy for downstream
  auto dbg_proxy = n1_conn->get_connection_handle_debug_info(proxy_node.get_app_id());
  CASE_EXPECT_TRUE(dbg_proxy.exists);
  CASE_EXPECT_GE(dbg_proxy.proxy_for_count, 1u);
  CASE_MSG_INFO() << "F.2: proxy handle proxy_for_count=" << dbg_proxy.proxy_for_count << '\n';

  // Verify downstream handle uses proxy
  auto dbg_down = n1_conn->get_connection_handle_debug_info(downstream.get_app_id());
  CASE_EXPECT_TRUE(dbg_down.exists);
  CASE_EXPECT_EQ(proxy_node.get_app_id(), dbg_down.proxy_bus_id);
  CASE_MSG_INFO() << "F.2: downstream proxy_bus_id=0x" << std::hex << dbg_down.proxy_bus_id << std::dec << '\n';

  // Step 1: Make proxy unready → cascade → downstream also unready
  n1_conn->set_handle_unready_by_bus_id(proxy_node.get_app_id());
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(proxy_node.get_app_id()));
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(downstream.get_app_id()));

  // Step 2: Send to downstream → message goes to pending queue (handle unready)
  setup_receive_callback(downstream, "F.2");
  int ret = send_test_message(node1, downstream.get_app_id(), "pending-msg-F2");
  CASE_MSG_INFO() << "F.2: send while unready returned " << ret << '\n';

  // Verify message is pending (not yet delivered)
  CASE_EXPECT_EQ(0, g_recovery_test_ctx.received_message_count);
  auto *ep_down = node1.get_endpoint(downstream.get_app_id());
  if (ep_down != nullptr) {
    CASE_EXPECT_GT(ep_down->get_pending_message_count(), static_cast<size_t>(0));
    CASE_MSG_INFO() << "F.2: pending count before proxy ready=" << ep_down->get_pending_message_count() << '\n';
  }

  // Step 3: Make proxy ready → cascade → downstream ready → pending messages flushed
  n1_conn->set_handle_ready_by_bus_id(proxy_node.get_app_id());
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(proxy_node.get_app_id()));
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(downstream.get_app_id()));

  // Step 4: Pump to let waker fire and deliver pending messages
  pump_apps_until([&]() { return g_recovery_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8),
                  proxy_node, node1, downstream);
  CASE_EXPECT_EQ(1, g_recovery_test_ctx.received_message_count);
  if (!g_recovery_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("pending-msg-F2", g_recovery_test_ctx.received_messages[0]);
  }
#endif
}

// ============================================================
// F.3: proxy_unready_cascade_downstream
// proxy unready → downstream also unready.
// ============================================================
CASE_TEST(atapp_error_recovery, proxy_unready_cascade_downstream) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs required, skip" << '\n';
  return;
#else
  reset_recovery_test_context();

  std::string conf1 = get_recovery_test_conf_path("atapp_test_recovery_1.yaml");
  std::string conf2 = get_recovery_test_conf_path("atapp_test_recovery_2.yaml");
  std::string conf3 = get_recovery_test_conf_path("atapp_test_recovery_3.yaml");

  if (check_recovery_and_skip_if_missing(conf1)) return;
  if (check_recovery_and_skip_if_missing(conf2)) return;
  if (check_recovery_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app proxy_node;
  atframework::atapp::app downstream;

  const char *args2[] = {"proxy", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, proxy_node.init(nullptr, 4, args2, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args3[] = {"downstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, downstream.init(nullptr, 4, args3, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b3 = downstream.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b3 && b3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), proxy_node, node1, downstream);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, proxy_disc, down_disc;
  setup_recovery_test_env(node1, proxy_node, downstream, n1_disc, proxy_disc, down_disc);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  pump_apps_until([&]() { return n1_conn->is_connection_handle_ready(downstream.get_app_id()); },
                  std::chrono::seconds(8), proxy_node, node1, downstream);

  // Verify both handles are ready
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(proxy_node.get_app_id()));
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(downstream.get_app_id()));

  // Verify flags before unready
  auto dbg_proxy_before = n1_conn->get_connection_handle_debug_info(proxy_node.get_app_id());
  auto dbg_down_before = n1_conn->get_connection_handle_debug_info(downstream.get_app_id());
  CASE_EXPECT_NE(0u, dbg_proxy_before.flags & kAtbusHandleFlagReady);
  CASE_EXPECT_NE(0u, dbg_down_before.flags & kAtbusHandleFlagReady);

  // Make proxy unready → cascade → downstream also unready
  n1_conn->set_handle_unready_by_bus_id(proxy_node.get_app_id());

  // Verify proxy is unready
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(proxy_node.get_app_id()));
  auto dbg_proxy_after = n1_conn->get_connection_handle_debug_info(proxy_node.get_app_id());
  CASE_EXPECT_EQ(0u, dbg_proxy_after.flags & kAtbusHandleFlagReady);

  // Verify downstream is ALSO unready (cascade)
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(downstream.get_app_id()));
  auto dbg_down_after = n1_conn->get_connection_handle_debug_info(downstream.get_app_id());
  CASE_EXPECT_EQ(0u, dbg_down_after.flags & kAtbusHandleFlagReady);

  CASE_MSG_INFO() << "F.3: proxy flags: 0x" << std::hex << dbg_proxy_after.flags << ", downstream flags: 0x"
                  << dbg_down_after.flags << std::dec << '\n';
#endif
}

// ============================================================
// F.4: proxy_removed_downstream_closed
// Call on_close_connection for proxy handle → remove_connection_handle →
// cascade → downstream's on_close_connection → both handles removed.
// ============================================================
CASE_TEST(atapp_error_recovery, proxy_removed_downstream_closed) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs required, skip" << '\n';
  return;
#else
  reset_recovery_test_context();

  std::string conf1 = get_recovery_test_conf_path("atapp_test_recovery_1.yaml");
  std::string conf2 = get_recovery_test_conf_path("atapp_test_recovery_2.yaml");
  std::string conf3 = get_recovery_test_conf_path("atapp_test_recovery_3.yaml");

  if (check_recovery_and_skip_if_missing(conf1)) return;
  if (check_recovery_and_skip_if_missing(conf2)) return;
  if (check_recovery_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app proxy_node;
  atframework::atapp::app downstream;

  const char *args2[] = {"proxy", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, proxy_node.init(nullptr, 4, args2, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args3[] = {"downstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, downstream.init(nullptr, 4, args3, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b3 = downstream.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b3 && b3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), proxy_node, node1, downstream);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, proxy_disc, down_disc;
  setup_recovery_test_env(node1, proxy_node, downstream, n1_disc, proxy_disc, down_disc);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  pump_apps_until([&]() { return n1_conn->is_connection_handle_ready(downstream.get_app_id()); },
                  std::chrono::seconds(8), proxy_node, node1, downstream);

  // Verify both handles exist and are ready
  CASE_EXPECT_TRUE(n1_conn->has_connection_handle(proxy_node.get_app_id()));
  CASE_EXPECT_TRUE(n1_conn->has_connection_handle(downstream.get_app_id()));
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(proxy_node.get_app_id()));
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(downstream.get_app_id()));

  // Verify proxy_for relationship
  auto dbg_proxy = n1_conn->get_connection_handle_debug_info(proxy_node.get_app_id());
  CASE_EXPECT_GE(dbg_proxy.proxy_for_count, 1u);

  // Get proxy's connection handle for on_close_connection
  auto *proxy_ep = node1.get_endpoint(proxy_node.get_app_id());
  CASE_EXPECT_TRUE(proxy_ep != nullptr);
  if (!proxy_ep) return;
  auto *proxy_conn_handle = proxy_ep->get_ready_connection_handle();
  CASE_EXPECT_TRUE(proxy_conn_handle != nullptr);
  if (!proxy_conn_handle) return;

  // Call on_close_connection for proxy → triggers remove_connection_handle →
  // cascade → downstream's on_close_connection (downstream has app_handle)
  n1_conn->on_close_connection(*proxy_conn_handle);

  // Verify both handles are removed
  CASE_EXPECT_FALSE(n1_conn->has_connection_handle(proxy_node.get_app_id()));
  CASE_EXPECT_FALSE(n1_conn->has_connection_handle(downstream.get_app_id()));
  CASE_MSG_INFO() << "F.4: both proxy and downstream handles removed after proxy close" << '\n';
#endif
}

// ============================================================
// F.5: proxy_removed_downstream_safe_cleanup
// Remove downstream handle first → then remove proxy →
// proxy's stale proxy_for reference is handled safely (no crash).
// ============================================================
CASE_TEST(atapp_error_recovery, proxy_removed_downstream_safe_cleanup) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs required, skip" << '\n';
  return;
#else
  reset_recovery_test_context();

  std::string conf1 = get_recovery_test_conf_path("atapp_test_recovery_1.yaml");
  std::string conf2 = get_recovery_test_conf_path("atapp_test_recovery_2.yaml");
  std::string conf3 = get_recovery_test_conf_path("atapp_test_recovery_3.yaml");

  if (check_recovery_and_skip_if_missing(conf1)) return;
  if (check_recovery_and_skip_if_missing(conf2)) return;
  if (check_recovery_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app proxy_node;
  atframework::atapp::app downstream;

  const char *args2[] = {"proxy", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, proxy_node.init(nullptr, 4, args2, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args3[] = {"downstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, downstream.init(nullptr, 4, args3, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b3 = downstream.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b3 && b3->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), proxy_node, node1, downstream);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, proxy_disc, down_disc;
  setup_recovery_test_env(node1, proxy_node, downstream, n1_disc, proxy_disc, down_disc);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  pump_apps_until([&]() { return n1_conn->is_connection_handle_ready(downstream.get_app_id()); },
                  std::chrono::seconds(8), proxy_node, node1, downstream);

  // Verify both handles exist
  CASE_EXPECT_TRUE(n1_conn->has_connection_handle(proxy_node.get_app_id()));
  CASE_EXPECT_TRUE(n1_conn->has_connection_handle(downstream.get_app_id()));

  // Verify proxy_for relationship
  auto dbg_proxy = n1_conn->get_connection_handle_debug_info(proxy_node.get_app_id());
  CASE_EXPECT_GE(dbg_proxy.proxy_for_count, 1u);

  // Step 1: Remove downstream handle first
  auto *down_ep = node1.get_endpoint(downstream.get_app_id());
  CASE_EXPECT_TRUE(down_ep != nullptr);
  if (!down_ep) return;
  auto *down_conn_handle = down_ep->get_ready_connection_handle();
  CASE_EXPECT_TRUE(down_conn_handle != nullptr);
  if (!down_conn_handle) return;

  n1_conn->on_close_connection(*down_conn_handle);
  CASE_EXPECT_FALSE(n1_conn->has_connection_handle(downstream.get_app_id()));
  CASE_MSG_INFO() << "F.5: downstream handle removed first" << '\n';

  // Step 2: Remove proxy handle — proxy_for_bus_id still references downstream
  // but downstream is already gone. The cascade should skip safely.
  auto *proxy_ep = node1.get_endpoint(proxy_node.get_app_id());
  CASE_EXPECT_TRUE(proxy_ep != nullptr);
  if (!proxy_ep) return;
  auto *proxy_conn_handle = proxy_ep->get_ready_connection_handle();
  CASE_EXPECT_TRUE(proxy_conn_handle != nullptr);
  if (!proxy_conn_handle) return;

  n1_conn->on_close_connection(*proxy_conn_handle);

  // Verify both handles are gone and no crash occurred
  CASE_EXPECT_FALSE(n1_conn->has_connection_handle(proxy_node.get_app_id()));
  CASE_EXPECT_FALSE(n1_conn->has_connection_handle(downstream.get_app_id()));
  CASE_MSG_INFO() << "F.5: proxy removed after downstream already removed — safe cleanup confirmed" << '\n';
#endif
}
