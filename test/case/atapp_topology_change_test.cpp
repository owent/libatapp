// Copyright 2026 atframework
// Test D: Topology change and lost topology tests
//
// Topology: node1(0x401) --proxy--> old_upstream(0x402) <--proxy-- target(0x404)
// For D.1-D.4: old_upstream(0x402) goes offline, new_upstream(0x403) takes over on :21902
// For D.5-D.9: simpler topology variations

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

struct topo_test_context {
  int received_message_count = 0;
  int forward_response_count = 0;
  int32_t last_forward_response_error = 0;
  std::vector<std::string> received_messages;
  std::vector<atframework::atapp::app_id_t> received_direct_source_ids;
};

static topo_test_context g_topo_test_ctx;

static void reset_topo_test_context() {
  g_topo_test_ctx.received_message_count = 0;
  g_topo_test_ctx.forward_response_count = 0;
  g_topo_test_ctx.last_forward_response_error = 0;
  g_topo_test_ctx.received_messages.clear();
  g_topo_test_ctx.received_direct_source_ids.clear();
}

static std::string get_topo_test_conf_path(const char *filename) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/" + filename;
}

static bool check_topo_and_skip_if_missing(const std::string &path) {
  if (!atfw::util::file_system::is_exist(path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << path << " not found, skip this test" << '\n';
    return true;
  }
  return false;
}

// Helper: pump multiple apps until condition or timeout
template <typename... Apps>
static void pump_apps_until(const std::function<bool()> &cond, std::chrono::seconds timeout, Apps &...apps) {
  auto start = atfw::util::time::time_utility::sys_now();
  auto end = start + timeout;
  while (!cond() && atfw::util::time::time_utility::sys_now() < end) {
    // Expand parameter pack to call run_noblock() on each app
    int dummy[] = {(apps.run_noblock(), 0)...};
    (void)dummy;
    CASE_THREAD_SLEEP_MS(1);
    atfw::util::time::time_utility::update();
  }
}

// Helper: set forward_request callback on target to collect received messages
static void setup_receive_callback(atframework::atapp::app &target, const char *label) {
  target.set_evt_on_forward_request([label](atframework::atapp::app &,
                                            const atframework::atapp::app::message_sender_t &sender,
                                            const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_topo_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_topo_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_topo_test_ctx.received_message_count;
    CASE_MSG_INFO() << label << ": received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });
}

// Helper: set forward_response callback on sender
static void setup_forward_response_callback(atframework::atapp::app &sender, const char *label) {
  sender.set_evt_on_forward_response([label](atframework::atapp::app &,
                                             const atframework::atapp::app::message_sender_t &,
                                             const atframework::atapp::app::message_t &, int32_t error_code) {
    g_topo_test_ctx.last_forward_response_error = error_code;
    ++g_topo_test_ctx.forward_response_count;
    CASE_MSG_INFO() << label << ": forward response error_code=" << error_code << '\n';
    return 0;
  });
}

// Helper: send a message and return the gsl::span used
static int send_test_message(atframework::atapp::app &from, atframework::atapp::app_id_t to, const char *data) {
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(data),
                                          static_cast<size_t>(strlen(data))};
  uint64_t seq = 0;
  return from.send_message(to, 100, msg_span, &seq);
}

// Set up 3-node topology:
//   node1(0x401) --proxy--> old_upstream(0x402) <--proxy-- target(0x404)
// on both bus_topology_registry and connector level
static void setup_3node_topology(atframework::atapp::app &node1, atframework::atapp::app &old_upstream,
                                 atframework::atapp::app &target) {
  auto bus1 = node1.get_bus_node();
  auto bus_up = old_upstream.get_bus_node();
  auto bus_t = target.get_bus_node();

  if (bus_up && bus_up->get_topology_registry()) {
    bus_up->get_topology_registry()->update_peer(node1.get_app_id(), old_upstream.get_app_id(), nullptr);
    bus_up->get_topology_registry()->update_peer(target.get_app_id(), old_upstream.get_app_id(), nullptr);
  }
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(old_upstream.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(target.get_app_id(), old_upstream.get_app_id(), nullptr);
  }
  if (bus_t && bus_t->get_topology_registry()) {
    bus_t->get_topology_registry()->update_peer(old_upstream.get_app_id(), 0, nullptr);
    bus_t->get_topology_registry()->update_peer(node1.get_app_id(), old_upstream.get_app_id(), nullptr);
  }

  auto up_conn = old_upstream.get_atbus_connector();
  if (up_conn) {
    up_conn->update_topology_peer(node1.get_app_id(), old_upstream.get_app_id(), nullptr);
    up_conn->update_topology_peer(target.get_app_id(), old_upstream.get_app_id(), nullptr);
  }
  auto n1_conn = node1.get_atbus_connector();
  if (n1_conn) {
    n1_conn->update_topology_peer(old_upstream.get_app_id(), 0, nullptr);
    n1_conn->update_topology_peer(target.get_app_id(), old_upstream.get_app_id(), nullptr);
  }
  auto t_conn = target.get_atbus_connector();
  if (t_conn) {
    t_conn->update_topology_peer(old_upstream.get_app_id(), 0, nullptr);
    t_conn->update_topology_peer(node1.get_app_id(), old_upstream.get_app_id(), nullptr);
  }
}

// Inject all discovery for 3 apps
static void inject_3node_discovery(atframework::atapp::app &node1, atframework::atapp::app &old_upstream,
                                   atframework::atapp::app &target,
                                   atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &node1_disc,
                                   atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &upstream_disc,
                                   atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &target_disc) {
  node1_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  upstream_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  target_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    node1_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    old_upstream.pack(info);
    upstream_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    target.pack(info);
    target_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  atframework::atapp::app *apps[] = {&node1, &old_upstream, &target};
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> discs[] = {node1_disc, upstream_disc, target_disc};
  for (auto *app : apps) {
    for (auto &disc : discs) {
      app->get_etcd_module()->get_global_discovery().add_node(disc);
    }
  }
}

// Create endpoints for 3 apps
static void create_3node_endpoints(atframework::atapp::app &node1, atframework::atapp::app &old_upstream,
                                   atframework::atapp::app &target,
                                   const atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &node1_disc,
                                   const atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &upstream_disc,
                                   const atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &target_disc) {
  CASE_EXPECT_TRUE(node1.mutable_endpoint(upstream_disc));
  CASE_EXPECT_TRUE(node1.mutable_endpoint(target_disc));
  CASE_EXPECT_TRUE(old_upstream.mutable_endpoint(node1_disc));
  CASE_EXPECT_TRUE(old_upstream.mutable_endpoint(target_disc));
  CASE_EXPECT_TRUE(target.mutable_endpoint(node1_disc));
  CASE_EXPECT_TRUE(target.mutable_endpoint(upstream_disc));
}

#endif

}  // namespace

// ============================================================
// D.1: topology_change_new_upstream_not_connected
// update_topology_peer switches to new upstream; new upstream has discovery
// but bus connection not yet established. After pump, connection establishes
// and message delivered through new upstream.
// ============================================================
CASE_TEST(atapp_topology_change, topology_change_new_upstream_not_connected) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_proxy_bus_id() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf3 = get_topo_test_conf_path("atapp_test_topo_3.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf3)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app target;

  // ---- Phase 1: old_upstream(0x402) alive, send works ----
  {
    atframework::atapp::app old_upstream;
    const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
    CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));

    const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
    CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));

    const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
    CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

    // Wait for bus connections
    pump_apps_until(
        [&]() {
          auto b1 = node1.get_bus_node();
          auto bt = target.get_bus_node();
          return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);

    // Inject discovery, setup topology, create endpoints
    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
    inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
    setup_3node_topology(node1, old_upstream, target);
    create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

    // Wait for target handle ready on node1
    pump_apps_until(
        [&]() {
          auto *ep = node1.get_endpoint(target.get_app_id());
          return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);

    // Verify message via old upstream
    setup_receive_callback(target, "D.1");
    CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "via-old-upstream-D1"));

    pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8),
                    old_upstream, node1, target);
    CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
    if (!g_topo_test_ctx.received_messages.empty()) {
      CASE_EXPECT_EQ("via-old-upstream-D1", g_topo_test_ctx.received_messages[0]);
    }

    // old_upstream destructs → releases :21902
  }

  // Let node1 and target process disconnect events
  for (int i = 0; i < 20; ++i) {
    node1.run_noblock();
    target.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // ---- Phase 2: new_upstream(0x403) starts on :21902 ----
  // DO NOT pump yet — bus connection not established
  atframework::atapp::app new_upstream;
  const char *args3[] = {"new_upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, new_upstream.init(nullptr, 4, args3, nullptr));

  // Create discovery for new_upstream
  auto new_up_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto node1_disc2 = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto target_disc2 = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  {
    atapp::protocol::atapp_discovery info;
    new_upstream.pack(info);
    new_up_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    node1_disc2->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    target.pack(info);
    target_disc2->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  // Inject discovery on all nodes
  node1.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  target.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(node1_disc2);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(target_disc2);

  // Update bus topology registries for new upstream
  auto bus1 = node1.get_bus_node();
  auto bust = target.get_bus_node();
  auto bus_nu = new_upstream.get_bus_node();
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (bust && bust->get_topology_registry()) {
    bust->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
    bust->get_topology_registry()->update_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (bus_nu && bus_nu->get_topology_registry()) {
    bus_nu->get_topology_registry()->update_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
    bus_nu->get_topology_registry()->update_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  // Create endpoints for new_upstream
  CASE_EXPECT_TRUE(node1.mutable_endpoint(new_up_disc));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(node1_disc2));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(target_disc2));
  CASE_EXPECT_TRUE(target.mutable_endpoint(new_up_disc));

  // Update connector topology — this triggers the update_topology_peer path
  auto n1_conn = node1.get_atbus_connector();
  auto t_conn = target.get_atbus_connector();
  auto nu_conn = new_upstream.get_atbus_connector();
  if (n1_conn) {
    n1_conn->update_topology_peer(new_upstream.get_app_id(), 0, nullptr);
    n1_conn->update_topology_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (t_conn) {
    t_conn->update_topology_peer(new_upstream.get_app_id(), 0, nullptr);
    t_conn->update_topology_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (nu_conn) {
    nu_conn->update_topology_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
    nu_conn->update_topology_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  // Now pump — bus auto-reconnects to :21902 (new_upstream)
  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), new_upstream, node1, target);

  // Wait for target handle ready on node1
  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(target.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), new_upstream, node1, target);

  // Verify handle is ready (proxy_bus_id may be 0 if bus has direct path)
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_MSG_INFO() << "D.1: handle ready=" << n1_conn->is_connection_handle_ready(target.get_app_id())
                    << " proxy_bus_id=0x" << std::hex
                    << n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()) << std::dec << '\n';
  }

  // Send via new upstream
  reset_topo_test_context();
  setup_receive_callback(target, "D.1");
  CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "via-new-upstream-D1"));

  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), new_upstream,
                  node1, target);

  CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
  if (!g_topo_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("via-new-upstream-D1", g_topo_test_ctx.received_messages[0]);
  }
  if (!g_topo_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(new_upstream.get_app_id(), g_topo_test_ctx.received_direct_source_ids[0]);
  }
#endif
}

// ============================================================
// D.2: topology_change_new_upstream_already_connected
// update_topology_peer switches to new upstream; new upstream already
// connected as bus endpoint → seamless switch, handle stays ready.
// ============================================================
CASE_TEST(atapp_topology_change, topology_change_new_upstream_already_connected) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_proxy_bus_id() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf3 = get_topo_test_conf_path("atapp_test_topo_3.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf3)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app target;
  uint64_t old_upstream_id = 0;

  // ---- Phase 1: old_upstream(0x402) alive, send works ----
  {
    atframework::atapp::app old_upstream;
    const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
    CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
    old_upstream_id = old_upstream.get_app_id();

    const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
    CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));

    const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
    CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

    pump_apps_until(
        [&]() {
          auto b1 = node1.get_bus_node();
          auto bt = target.get_bus_node();
          return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);

    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
    inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
    setup_3node_topology(node1, old_upstream, target);
    create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

    pump_apps_until(
        [&]() {
          auto *ep = node1.get_endpoint(target.get_app_id());
          return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);

    setup_receive_callback(target, "D.2");
    CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "before-switch-D2"));

    pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8),
                    old_upstream, node1, target);
    CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
    if (!g_topo_test_ctx.received_messages.empty()) {
      CASE_EXPECT_EQ("before-switch-D2", g_topo_test_ctx.received_messages[0]);
    }
    CASE_MSG_INFO() << "D.2: Phase 1 done, direct_source=0x" << std::hex << old_upstream_id << std::dec << '\n';
  }

  // Process disconnect events
  for (int i = 0; i < 10; ++i) {
    node1.run_noblock();
    target.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // ---- Phase 2: new_upstream(0x403) starts on :21902, pump FIRST to establish bus ----
  atframework::atapp::app new_upstream;
  const char *args3[] = {"new_upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, new_upstream.init(nullptr, 4, args3, nullptr));

  // Pump first — bus auto-reconnects to :21902 which is now new_upstream(0x403)
  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), new_upstream, node1, target);

  // Verify reconnected to 0x403
  {
    auto b1 = node1.get_bus_node();
    if (b1 && b1->get_upstream_endpoint()) {
      CASE_EXPECT_EQ(new_upstream.get_app_id(), b1->get_upstream_endpoint()->get_id());
    }
  }

  // Now inject discovery and update topology — bus already connected
  auto new_up_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto node1_disc2 = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto target_disc2 = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  {
    atapp::protocol::atapp_discovery info;
    new_upstream.pack(info);
    new_up_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    node1_disc2->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    target.pack(info);
    target_disc2->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  node1.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  target.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(node1_disc2);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(target_disc2);

  // Update bus topology registries
  auto bus1 = node1.get_bus_node();
  auto bust = target.get_bus_node();
  auto bus_nu = new_upstream.get_bus_node();
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (bust && bust->get_topology_registry()) {
    bust->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
    bust->get_topology_registry()->update_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (bus_nu && bus_nu->get_topology_registry()) {
    bus_nu->get_topology_registry()->update_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
    bus_nu->get_topology_registry()->update_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  CASE_EXPECT_TRUE(node1.mutable_endpoint(new_up_disc));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(node1_disc2));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(target_disc2));
  CASE_EXPECT_TRUE(target.mutable_endpoint(new_up_disc));

  // Update connector topology — bus IS already connected, so handle should be set ready
  // synchronously via check_atbus_endpoint_available → set_handle_ready
  auto n1_conn = node1.get_atbus_connector();
  auto t_conn = target.get_atbus_connector();
  auto nu_conn = new_upstream.get_atbus_connector();
  if (n1_conn) {
    n1_conn->update_topology_peer(new_upstream.get_app_id(), 0, nullptr);
    n1_conn->update_topology_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (t_conn) {
    t_conn->update_topology_peer(new_upstream.get_app_id(), 0, nullptr);
    t_conn->update_topology_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (nu_conn) {
    nu_conn->update_topology_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
    nu_conn->update_topology_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  // Handle should be ready immediately (bus already connected)
  auto *ep_t = node1.get_endpoint(target.get_app_id());
  CASE_EXPECT_TRUE(ep_t != nullptr);
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_EQ(new_upstream.get_app_id(), n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()));
    CASE_MSG_INFO() << "D.2: handle ready=" << n1_conn->is_connection_handle_ready(target.get_app_id()) << " proxy=0x"
                    << std::hex << n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()) << std::dec << '\n';
  }

  // Send via new upstream — should work seamlessly
  reset_topo_test_context();
  setup_receive_callback(target, "D.2");
  CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "after-switch-D2"));

  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), new_upstream,
                  node1, target);

  CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
  if (!g_topo_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("after-switch-D2", g_topo_test_ctx.received_messages[0]);
  }
  if (!g_topo_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(new_upstream.get_app_id(), g_topo_test_ctx.received_direct_source_ids[0]);
  }
#endif
}

// ============================================================
// D.3: topology_change_discovery_missing_then_arrive
// update_topology_peer to new upstream, but new upstream has no discovery
// yet → kWaitForDiscoveryToConnect. Then discovery arrives → reconnect.
// ============================================================
CASE_TEST(atapp_topology_change, topology_change_discovery_missing_then_arrive) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_proxy_bus_id() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf3 = get_topo_test_conf_path("atapp_test_topo_3.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf3)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app target;
  atframework::atapp::app_id_t old_upstream_id = 0;

  // ---- Phase 1: old_upstream(0x402) alive ----
  {
    atframework::atapp::app old_upstream;
    const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
    CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
    old_upstream_id = old_upstream.get_app_id();

    const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
    CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));

    const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
    CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

    pump_apps_until(
        [&]() {
          auto b1 = node1.get_bus_node();
          auto bt = target.get_bus_node();
          return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);

    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
    inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
    setup_3node_topology(node1, old_upstream, target);
    create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

    pump_apps_until(
        [&]() {
          auto *ep = node1.get_endpoint(target.get_app_id());
          return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);
  }

  // Process disconnect events
  for (int i = 0; i < 10; ++i) {
    node1.run_noblock();
    target.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Verify degraded state: bus lost upstream endpoint
  {
    auto b1 = node1.get_bus_node();
    CASE_EXPECT_TRUE(b1 != nullptr);
    if (b1) {
      CASE_EXPECT_TRUE(b1->get_upstream_endpoint() == nullptr);
    }
  }
  auto n1_conn = node1.get_atbus_connector();
  if (n1_conn) {
    // Remove old upstream from topology → kLostTopology set
    n1_conn->remove_topology_peer(old_upstream_id);
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(old_upstream_id));
  }

  // ---- Phase 2: new_upstream(0x403) starts, pump until bus connected ----
  atframework::atapp::app new_upstream;
  const char *args3[] = {"new_upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, new_upstream.init(nullptr, 4, args3, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), new_upstream, node1, target);

  // Update bus topology registries (NO discovery for new_upstream yet)
  auto bus1 = node1.get_bus_node();
  auto bust = target.get_bus_node();
  auto bus_nu = new_upstream.get_bus_node();
  if (bus1 && bus1->get_topology_registry()) {
    bus1->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
    bus1->get_topology_registry()->update_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (bust && bust->get_topology_registry()) {
    bust->get_topology_registry()->update_peer(new_upstream.get_app_id(), 0, nullptr);
    bust->get_topology_registry()->update_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  if (bus_nu && bus_nu->get_topology_registry()) {
    bus_nu->get_topology_registry()->update_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
    bus_nu->get_topology_registry()->update_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  // Update connector topology WITHOUT new_upstream discovery
  if (n1_conn) {
    n1_conn->update_topology_peer(new_upstream.get_app_id(), 0, nullptr);
    n1_conn->update_topology_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  auto t_conn = target.get_atbus_connector();
  if (t_conn) {
    t_conn->update_topology_peer(new_upstream.get_app_id(), 0, nullptr);
    t_conn->update_topology_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
  }
  auto nu_conn = new_upstream.get_atbus_connector();

  // Verify after topology update: new upstream handle exists, no kLostTopology
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->has_connection_handle(new_upstream.get_app_id()));
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(new_upstream.get_app_id()));
  }

  // Send message — should go to pending (handle not ready)
  setup_receive_callback(target, "D.3");
  send_test_message(node1, target.get_app_id(), "discovery-delayed-D3");

  // Tick to verify pending
  for (int i = 0; i < 32; ++i) {
    node1.tick();
  }
  CASE_EXPECT_EQ(0, g_topo_test_ctx.received_message_count);
  CASE_MSG_INFO() << "D.3: received after tick (no discovery)=" << g_topo_test_ctx.received_message_count << '\n';

  // Now inject discovery for new_upstream → triggers on_discovery_event(kPut) →
  // resume_handle_discovery → try_direct_reconnect
  auto new_up_disc = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto node1_disc2 = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  auto target_disc2 = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  {
    atapp::protocol::atapp_discovery info;
    new_upstream.pack(info);
    new_up_disc->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    node1.pack(info);
    node1_disc2->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    target.pack(info);
    target_disc2->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  // Inject discovery for new_upstream on all nodes
  node1.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  target.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(new_up_disc);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(node1_disc2);
  new_upstream.get_etcd_module()->get_global_discovery().add_node(target_disc2);

  // Create endpoints for new_upstream
  CASE_EXPECT_TRUE(node1.mutable_endpoint(new_up_disc));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(node1_disc2));
  CASE_EXPECT_TRUE(new_upstream.mutable_endpoint(target_disc2));
  CASE_EXPECT_TRUE(target.mutable_endpoint(new_up_disc));

  if (nu_conn) {
    nu_conn->update_topology_peer(node1.get_app_id(), new_upstream.get_app_id(), nullptr);
    nu_conn->update_topology_peer(target.get_app_id(), new_upstream.get_app_id(), nullptr);
  }

  // Pump — discovery arrived, connection should establish
  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), new_upstream,
                  node1, target);

  CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
  if (!g_topo_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("discovery-delayed-D3", g_topo_test_ctx.received_messages[0]);
  }
  if (!g_topo_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(new_upstream.get_app_id(), g_topo_test_ctx.received_direct_source_ids[0]);
  }

  // Verify recovered: new upstream handle exists, no kLostTopology
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->has_connection_handle(new_upstream.get_app_id()));
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(new_upstream.get_app_id()));
  }
#endif
}

// ============================================================
// D.4: topology_change_discovery_timeout_cleanup
// update_topology_peer to new upstream with no discovery →
// set_sys_now to timeout → handle removed → pending fails.
// ============================================================
CASE_TEST(atapp_topology_change, topology_change_discovery_timeout_cleanup) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app target;
  atframework::atapp::app_id_t old_upstream_id = 0;

  // Phase 1: old_upstream alive, connect, setup, then destruct
  {
    atframework::atapp::app old_upstream;
    const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
    CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
    old_upstream_id = old_upstream.get_app_id();

    const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
    CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));

    const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
    CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

    pump_apps_until(
        [&]() {
          auto b1 = node1.get_bus_node();
          auto bt = target.get_bus_node();
          return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);

    atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
    inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
    setup_3node_topology(node1, old_upstream, target);
    create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

    pump_apps_until(
        [&]() {
          auto *ep = node1.get_endpoint(target.get_app_id());
          return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
        },
        std::chrono::seconds(8), old_upstream, node1, target);
  }

  // Process disconnect
  for (int i = 0; i < 10; ++i) {
    node1.run_noblock();
    target.run_noblock();
    CASE_THREAD_SLEEP_MS(10);
  }

  // Sync jiffies timer
  node1.tick();

  // Verify degraded state: bus lost upstream endpoint
  {
    auto b1 = node1.get_bus_node();
    CASE_EXPECT_TRUE(b1 != nullptr);
    if (b1) {
      CASE_EXPECT_TRUE(b1->get_upstream_endpoint() == nullptr);
    }
  }
  auto n1_conn = node1.get_atbus_connector();
  if (n1_conn) {
    // Remove old upstream from topology → kLostTopology set
    n1_conn->remove_topology_peer(old_upstream_id);
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(old_upstream_id));
  }

  // Update connector topology to point target at non-existent new upstream (0x403)
  if (n1_conn) {
    n1_conn->update_topology_peer(target.get_app_id(), 0x403, nullptr);

    // Verify: bus still has no upstream, old upstream still flagged
    {
      auto b1 = node1.get_bus_node();
      CASE_EXPECT_TRUE(b1 != nullptr);
      if (b1) {
        CASE_EXPECT_TRUE(b1->get_upstream_endpoint() == nullptr);
      }
    }
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(old_upstream_id));
  }

  // Send message → pending (handle unready)
  setup_forward_response_callback(node1, "D.4");
  setup_receive_callback(target, "D.4");
  send_test_message(node1, target.get_app_id(), "will-timeout-D4");

  // Advance time with set_sys_now + tick — reconnect retries exhaust,
  // pending message times out with EN_ATBUS_ERR_NODE_TIMEOUT
  auto now = atframework::atapp::app::get_sys_now();

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    node1.tick();
    if (g_topo_test_ctx.forward_response_count > 0) {
      CASE_MSG_INFO() << "D.4: forward response after " << i * 50 << "ms" << '\n';
      break;
    }
  }

  // Verify: pending message failed with timeout, never delivered
  CASE_EXPECT_GT(g_topo_test_ctx.forward_response_count, 0);
  CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_topo_test_ctx.last_forward_response_error);
  CASE_EXPECT_EQ(0, g_topo_test_ctx.received_message_count);

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// D.5: topology_lost_recover_before_timeout
// remove_topology_peer → kLostTopology=true → before timeout,
// update_topology_peer restores → kLostTopology cleared → comm OK.
// ============================================================
CASE_TEST(atapp_topology_change, topology_lost_recover_before_timeout) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app old_upstream;
  atframework::atapp::app target;

  const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
  CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
  inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
  setup_3node_topology(node1, old_upstream, target);
  create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(target.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  setup_receive_callback(target, "D.5");

  // Send before topology loss — should work
  CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "before-loss-D5"));
  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), old_upstream,
                  node1, target);
  CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);

  // Sync jiffies timer
  node1.tick();

  // Verify handle is ready before removal
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(target.get_app_id()));
  }

  // Remove topology for target → kLostTopology set
  if (n1_conn) {
    n1_conn->remove_topology_peer(target.get_app_id());
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(target.get_app_id()));
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
  }

  // Advance time partially (10s < 32s timeout)
  auto now = atframework::atapp::app::get_sys_now();
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(10));
  node1.tick();

  // Handle should still exist with kLostTopology still set
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->has_connection_handle(target.get_app_id()));
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(target.get_app_id()));
  }

  // Restore topology BEFORE timeout → kLostTopology cleared
  if (n1_conn) {
    n1_conn->update_topology_peer(target.get_app_id(), old_upstream.get_app_id(), nullptr);
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(target.get_app_id()));
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_EQ(old_upstream.get_app_id(), n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()));
  }

  // Restore time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());

  // Send again — should still work
  CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "after-restore-D5"));
  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 2; }, std::chrono::seconds(8), old_upstream,
                  node1, target);

  CASE_EXPECT_EQ(2, g_topo_test_ctx.received_message_count);
  if (g_topo_test_ctx.received_messages.size() >= 2) {
    CASE_EXPECT_EQ("before-loss-D5", g_topo_test_ctx.received_messages[0]);
    CASE_EXPECT_EQ("after-restore-D5", g_topo_test_ctx.received_messages[1]);
  }
#endif
}

// ============================================================
// D.6: topology_lost_timeout_cleanup
// remove_topology_peer → set_sys_now past lost_topology_timeout →
// handle force-removed → pending message fails.
// ============================================================
CASE_TEST(atapp_topology_change, topology_lost_timeout_cleanup) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app old_upstream;
  atframework::atapp::app target;

  const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
  CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
  inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
  setup_3node_topology(node1, old_upstream, target);
  create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(target.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);

  // Sync jiffies timer
  node1.tick();

  // Verify handle is ready before removal
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
  }

  // Remove topology for target → kLostTopology set
  if (n1_conn) {
    n1_conn->remove_topology_peer(target.get_app_id());
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(target.get_app_id()));
    CASE_EXPECT_TRUE(n1_conn->has_connection_handle(target.get_app_id()));
  }

  // Simulate handle becoming unready
  if (n1_conn) {
    n1_conn->set_handle_unready_by_bus_id(target.get_app_id());
    CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(target.get_app_id()));
  }

  // Send message → pending
  setup_forward_response_callback(node1, "D.6");
  send_test_message(node1, target.get_app_id(), "will-fail-D6");

  // Advance time past lost_topology_timeout (32s)
  auto now = atframework::atapp::app::get_sys_now();
  bool handle_removed = false;

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    node1.tick();
    if (!handle_removed && n1_conn && !n1_conn->has_connection_handle(target.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "D.6: handle removed after " << i * 50 << "ms" << '\n';
    }
    if (handle_removed && g_topo_test_ctx.forward_response_count > 0) {
      break;
    }
  }

  CASE_EXPECT_TRUE(handle_removed);
  CASE_EXPECT_GT(g_topo_test_ctx.forward_response_count, 0);
  if (g_topo_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_topo_test_ctx.last_forward_response_error);
  }

  // After GC timeout, endpoint should be cleaned up
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(200));
  node1.tick();

  auto *ep_t = node1.get_endpoint(target.get_app_id());
  CASE_EXPECT_TRUE(!ep_t);

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// D.7: topology_lost_ready_handle_force_remove
// handle kReady + kLostTopology → set_sys_now past timeout →
// handle force-removed despite being ready (safety net mechanism).
// ============================================================
CASE_TEST(atapp_topology_change, topology_lost_ready_handle_force_remove) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app old_upstream;
  atframework::atapp::app target;

  const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
  CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
  inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
  setup_3node_topology(node1, old_upstream, target);
  create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(target.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);

  // Verify handle is ready
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
  }

  // Sync jiffies timer
  node1.tick();

  // Remove topology → kLostTopology set, but handle remains READY
  if (n1_conn) {
    n1_conn->remove_topology_peer(target.get_app_id());
    CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(target.get_app_id()));
    // Handle should still be ready (kReady + kLostTopology)
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_MSG_INFO() << "D.7: kReady+kLostTopology, ready=" << n1_conn->is_connection_handle_ready(target.get_app_id())
                    << '\n';
  }

  // Advance time past lost_topology_timeout (32s)
  auto now = atframework::atapp::app::get_sys_now();
  bool handle_removed = false;

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    node1.tick();
    if (!handle_removed && n1_conn && !n1_conn->has_connection_handle(target.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "D.7: handle removed after " << i * 50 << "ms" << '\n';
      break;
    }
  }

  CASE_EXPECT_TRUE(handle_removed);
  CASE_MSG_INFO() << "D.7: handle_removed=" << handle_removed << '\n';

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// D.8: topology_same_upstream_noop
// update_topology_peer with unchanged upstream_id → only calls
// try_direct_reconnect, handle state unchanged.
// ============================================================
CASE_TEST(atapp_topology_change, topology_same_upstream_noop) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs needed, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app old_upstream;
  atframework::atapp::app target;

  const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
  CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
  inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
  setup_3node_topology(node1, old_upstream, target);
  create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(target.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);

  // Verify state before: handle is ready with proxy == old_upstream
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_EQ(old_upstream.get_app_id(), n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()));
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(target.get_app_id()));
  }

  // Call update_topology_peer with SAME upstream → should be noop
  if (n1_conn) {
    n1_conn->update_topology_peer(target.get_app_id(), old_upstream.get_app_id(), nullptr);
  }

  // Verify state unchanged after same-upstream update
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_EQ(old_upstream.get_app_id(), n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()));
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(target.get_app_id()));
  }

  // Verify message still works
  setup_receive_callback(target, "D.8");
  CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "same-upstream-D8"));
  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), old_upstream,
                  node1, target);
  CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
  if (!g_topo_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("same-upstream-D8", g_topo_test_ctx.received_messages[0]);
  }
#endif
}

// ============================================================
// D.9: topology_proxy_in_new_upstream_path
// update_topology_peer where current proxy_bus_id is still on the
// new upstream's chain → no recalculation, keep existing proxy.
// ============================================================
CASE_TEST(atapp_topology_change, topology_proxy_in_new_upstream_path) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "Debug-only APIs needed, skip" << '\n';
  return;
#else
  reset_topo_test_context();

  std::string conf1 = get_topo_test_conf_path("atapp_test_topo_1.yaml");
  std::string conf2 = get_topo_test_conf_path("atapp_test_topo_2.yaml");
  std::string conf4 = get_topo_test_conf_path("atapp_test_topo_4.yaml");

  if (check_topo_and_skip_if_missing(conf1)) return;
  if (check_topo_and_skip_if_missing(conf2)) return;
  if (check_topo_and_skip_if_missing(conf4)) return;

  atframework::atapp::app node1;
  atframework::atapp::app old_upstream;
  atframework::atapp::app target;

  const char *args_up[] = {"old_upstream", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, old_upstream.init(nullptr, 4, args_up, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args4[] = {"target", "-c", conf4.c_str(), "start"};
  CASE_EXPECT_EQ(0, target.init(nullptr, 4, args4, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto bt = target.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && bt && bt->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1d, upd, td;
  inject_3node_discovery(node1, old_upstream, target, n1d, upd, td);
  setup_3node_topology(node1, old_upstream, target);
  create_3node_endpoints(node1, old_upstream, target, n1d, upd, td);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(target.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), old_upstream, node1, target);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);

  // Current proxy for target should be old_upstream(0x402)
  if (n1_conn) {
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_EQ(old_upstream.get_app_id(), n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()));
  }

  // Update bus topology so target's chain is: target(0x404) → 0x402 → (root)
  // i.e., the old_upstream IS in the chain of the "new" upstream info
  // We tell the connector that target's upstream is now some new_upstream_id (e.g. 0x402 itself)
  // but proxy is still 0x402 which IS in the path → code detects and returns early
  auto bus1 = node1.get_bus_node();
  if (bus1 && bus1->get_topology_registry()) {
    // Add a virtual upstream 0x405 whose upstream is 0x402
    bus1->get_topology_registry()->update_peer(0x405, old_upstream.get_app_id(), nullptr);
    // Now set target's upstream to 0x405 (whose chain includes 0x402)
    bus1->get_topology_registry()->update_peer(target.get_app_id(), 0x405, nullptr);
  }

  // update_topology_peer: target upstream changes to 0x405
  // Code walks 0x405's chain: 0x405 → 0x402, finds proxy_bus_id(0x402) → returns early
  if (n1_conn) {
    n1_conn->update_topology_peer(target.get_app_id(), 0x405, nullptr);
  }

  // Proxy should be unchanged (still old_upstream 0x402)
  if (n1_conn) {
    CASE_EXPECT_EQ(old_upstream.get_app_id(), n1_conn->get_connection_handle_proxy_bus_id(target.get_app_id()));
    CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(target.get_app_id()));
    CASE_EXPECT_FALSE(n1_conn->has_lost_topology_flag(target.get_app_id()));
  }

  // Verify message still works through old proxy
  setup_receive_callback(target, "D.9");
  CASE_EXPECT_EQ(0, send_test_message(node1, target.get_app_id(), "proxy-in-path-D9"));
  pump_apps_until([&]() { return g_topo_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), old_upstream,
                  node1, target);
  CASE_EXPECT_EQ(1, g_topo_test_ctx.received_message_count);
  if (!g_topo_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("proxy-in-path-D9", g_topo_test_ctx.received_messages[0]);
  }
#endif
}
