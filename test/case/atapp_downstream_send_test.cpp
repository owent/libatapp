// Copyright 2026 atframework
// Test C: Downstream node data send tests
//
// Topology: upstream(0x301) <--proxy-- downstream(0x302)
// upstream sends messages to downstream

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

struct downstream_test_context {
  int received_message_count = 0;
  int forward_response_count = 0;
  int32_t last_forward_response_error = 0;
  std::vector<std::string> received_messages;
  std::vector<atframework::atapp::app_id_t> received_direct_source_ids;
};

static downstream_test_context g_downstream_test_ctx;

static void reset_downstream_test_context() {
  g_downstream_test_ctx.received_message_count = 0;
  g_downstream_test_ctx.forward_response_count = 0;
  g_downstream_test_ctx.last_forward_response_error = 0;
  g_downstream_test_ctx.received_messages.clear();
  g_downstream_test_ctx.received_direct_source_ids.clear();
}

static std::string get_downstream_test_conf_path(const char *filename) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/" + filename;
}

static bool check_downstream_and_skip_if_missing(const std::string &path) {
  if (!atfw::util::file_system::is_exist(path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << path << " not found, skip this test" << '\n';
    return true;
  }
  return false;
}

struct downstream_two_node_apps {
  atframework::atapp::app upstream;
  atframework::atapp::app downstream;

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> upstream_discovery;
  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> downstream_discovery;

  bool init_all() {
    std::string conf1 = get_downstream_test_conf_path("atapp_test_downstream_1.yaml");
    std::string conf2 = get_downstream_test_conf_path("atapp_test_downstream_2.yaml");

    if (check_downstream_and_skip_if_missing(conf1)) return false;
    if (check_downstream_and_skip_if_missing(conf2)) return false;

    // Init upstream first (it needs to be listening before downstream connects)
    const char *args_up[] = {"upstream", "-c", conf1.c_str(), "start"};
    if (upstream.init(nullptr, 4, args_up, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init upstream app" << '\n';
      return false;
    }

    const char *args_down[] = {"downstream", "-c", conf2.c_str(), "start"};
    if (downstream.init(nullptr, 4, args_down, nullptr) != 0) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << "Failed to init downstream app" << '\n';
      return false;
    }

    return true;
  }

  void pump_until(const std::function<bool()> &cond, std::chrono::seconds timeout = std::chrono::seconds(8)) {
    auto start = atfw::util::time::time_utility::sys_now();
    auto end = start + timeout;
    while (!cond() && atfw::util::time::time_utility::sys_now() < end) {
      upstream.run_noblock();
      downstream.run_noblock();
      CASE_THREAD_SLEEP_MS(1);
      atfw::util::time::time_utility::update();
    }
  }

  void wait_for_downstream_connected() {
    pump_until(
        [this]() {
          auto bus_down = downstream.get_bus_node();
          return bus_down && bus_down->get_upstream_endpoint() != nullptr;
        },
        std::chrono::seconds(8));
  }

  void inject_all_discovery() {
    upstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    downstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

    {
      atapp::protocol::atapp_discovery info;
      upstream.pack(info);
      upstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }
    {
      atapp::protocol::atapp_discovery info;
      downstream.pack(info);
      downstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    auto &up_gd = upstream.get_etcd_module()->get_global_discovery();
    auto &down_gd = downstream.get_etcd_module()->get_global_discovery();

    up_gd.add_node(upstream_discovery);
    up_gd.add_node(downstream_discovery);

    down_gd.add_node(upstream_discovery);
    down_gd.add_node(downstream_discovery);
  }

  void inject_discovery_without_downstream() {
    upstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();

    {
      atapp::protocol::atapp_discovery info;
      upstream.pack(info);
      upstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    auto &up_gd = upstream.get_etcd_module()->get_global_discovery();
    auto &down_gd = downstream.get_etcd_module()->get_global_discovery();

    up_gd.add_node(upstream_discovery);
    down_gd.add_node(upstream_discovery);
  }

  void inject_downstream_discovery_later() {
    downstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    {
      atapp::protocol::atapp_discovery info;
      downstream.pack(info);
      downstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
    }

    auto &up_gd = upstream.get_etcd_module()->get_global_discovery();
    auto &down_gd = downstream.get_etcd_module()->get_global_discovery();

    up_gd.add_node(downstream_discovery);
    down_gd.add_node(downstream_discovery);
  }

  void setup_topology() {
    auto bus_up = upstream.get_bus_node();
    auto bus_down = downstream.get_bus_node();

    // downstream's upstream is upstream(0x301)
    if (bus_up && bus_up->get_topology_registry()) {
      bus_up->get_topology_registry()->update_peer(downstream.get_app_id(), upstream.get_app_id(), nullptr);
    }
    if (bus_down && bus_down->get_topology_registry()) {
      bus_down->get_topology_registry()->update_peer(upstream.get_app_id(), 0, nullptr);
    }

    auto up_connector = upstream.get_atbus_connector();
    if (up_connector) {
      up_connector->update_topology_peer(downstream.get_app_id(), upstream.get_app_id(), nullptr);
    }

    auto down_connector = downstream.get_atbus_connector();
    if (down_connector) {
      down_connector->update_topology_peer(upstream.get_app_id(), 0, nullptr);
    }
  }

  void create_endpoints() {
    CASE_EXPECT_TRUE(upstream.mutable_endpoint(downstream_discovery));
    CASE_EXPECT_TRUE(downstream.mutable_endpoint(upstream_discovery));
  }

  void pump_until_upstream_can_send() {
    pump_until(
        [this]() {
          auto *ep_down = upstream.get_endpoint(downstream.get_app_id());
          return ep_down != nullptr && ep_down->get_ready_connection_handle() != nullptr;
        },
        std::chrono::seconds(8));
  }
};

}  // namespace

// ============================================================
// C.1: downstream_not_connected_pending
// Bus connection is not yet established (no IO pump). Upstream creates
// endpoint for downstream and sends a message, which goes to pending
// queue. tick() alone does NOT deliver (no IO). run_noblock() processes
// IO, establishes bus connection, on_update_endpoint fires and sets
// handle ready, pending message gets delivered.
// ============================================================
CASE_TEST(atapp_downstream_send, downstream_not_connected_pending) {
  reset_downstream_test_context();

  downstream_two_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  // DO NOT pump here — bus connection is not yet established

  // Create discovery nodes manually
  apps.upstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  apps.downstream_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  {
    atapp::protocol::atapp_discovery info;
    apps.upstream.pack(info);
    apps.upstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
  }
  {
    atapp::protocol::atapp_discovery info;
    apps.downstream.pack(info);
    apps.downstream_discovery->copy_from(info, atapp::etcd_discovery_node::node_version());
  }

  // Inject upstream discovery on both sides
  auto &up_gd = apps.upstream.get_etcd_module()->get_global_discovery();
  auto &down_gd = apps.downstream.get_etcd_module()->get_global_discovery();
  up_gd.add_node(apps.upstream_discovery);
  down_gd.add_node(apps.upstream_discovery);

  // Inject downstream discovery on upstream side so endpoint can be created.
  // NOT injected on downstream side yet — will be injected later.
  up_gd.add_node(apps.downstream_discovery);

  // Setup topology
  apps.setup_topology();

  // Set up receive callback on downstream BEFORE creating endpoint
  apps.downstream.set_evt_on_forward_request([](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_downstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_downstream_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_downstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "C.1: downstream received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // send_message triggers mutable_endpoint → on_start_connect → try_connect_to →
  // on_start_connect_to_downstream_peer → check_atbus_endpoint_available → bus NOT connected
  // → handle set unready → message goes to pending queue via push_forward_message
  char msg_data[] = "pending-downstream-C1";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  apps.upstream.send_message(apps.downstream.get_app_id(), 100, msg_span, &seq);

  // Verify handle NOT ready (bus connection hasn't been processed)
  auto *ep_down = apps.upstream.get_endpoint(apps.downstream.get_app_id());
  CASE_EXPECT_TRUE(ep_down != nullptr);
  if (ep_down != nullptr) {
    CASE_EXPECT_TRUE(ep_down->get_ready_connection_handle() == nullptr);
    CASE_MSG_INFO() << "C.1: handle NOT ready (bus not connected)" << '\n';
  }

  // tick() processes timers but NOT IO — bus stays disconnected, message stays pending
  for (int i = 0; i < 64; ++i) {
    apps.upstream.tick();
    apps.downstream.tick();
  }
  CASE_EXPECT_EQ(0, g_downstream_test_ctx.received_message_count);
  CASE_MSG_INFO() << "C.1: received_count after tick=" << g_downstream_test_ctx.received_message_count << '\n';

  // Verify message is in pending queue
  ep_down = apps.upstream.get_endpoint(apps.downstream.get_app_id());
  CASE_EXPECT_TRUE(ep_down != nullptr);
  if (ep_down != nullptr) {
    CASE_MSG_INFO() << "C.1: pending count=" << ep_down->get_pending_message_count() << '\n';
    CASE_EXPECT_GT(ep_down->get_pending_message_count(), static_cast<size_t>(0));
  }

  // run_noblock() processes IO (shared uv_default_loop) → downstream TCP-connects to
  // upstream via proxy config → bus endpoint established → on_update_endpoint fires →
  // set_handle_ready → add_waker → pending message delivered on next tick
  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_downstream_test_ctx.received_message_count);
  if (!g_downstream_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("pending-downstream-C1", g_downstream_test_ctx.received_messages[0]);
  }
  if (!g_downstream_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(apps.upstream.get_app_id(), g_downstream_test_ctx.received_direct_source_ids[0]);
  }
}

// ============================================================
// C.2: downstream_connected_send_success
// Both nodes connected and discovery ready, send from upstream to
// downstream should succeed immediately.
// ============================================================
CASE_TEST(atapp_downstream_send, downstream_connected_send_success) {
  reset_downstream_test_context();

  downstream_two_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.wait_for_downstream_connected();
  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();

  // Wait for upstream's handle to downstream to become ready
  apps.pump_until_upstream_can_send();

  // Verify handle is ready
  auto *ep_down = apps.upstream.get_endpoint(apps.downstream.get_app_id());
  CASE_EXPECT_TRUE(ep_down != nullptr);
  if (ep_down != nullptr) {
    CASE_EXPECT_TRUE(ep_down->get_ready_connection_handle() != nullptr);
  }

  // Set up receive callback on downstream
  apps.downstream.set_evt_on_forward_request([](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_downstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_downstream_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_downstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "C.2: downstream received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // Send message — should be delivered immediately
  char msg_data[] = "downstream-send-C2";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.upstream.send_message(apps.downstream.get_app_id(), 100, msg_span, &seq));

  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_downstream_test_ctx.received_message_count);
  if (!g_downstream_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("downstream-send-C2", g_downstream_test_ctx.received_messages[0]);
  }
  if (!g_downstream_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(apps.upstream.get_app_id(), g_downstream_test_ctx.received_direct_source_ids[0]);
  }

  // Also test send by name
  char msg_data2[] = "downstream-by-name-C2";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  CASE_EXPECT_EQ(0, apps.upstream.send_message(apps.downstream.get_app_name(), 100, msg_span2, &seq));

  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 2; });

  CASE_EXPECT_EQ(2, g_downstream_test_ctx.received_message_count);
  if (g_downstream_test_ctx.received_messages.size() >= 2) {
    CASE_EXPECT_EQ("downstream-by-name-C2", g_downstream_test_ctx.received_messages[1]);
  }

  // === Downstream → Upstream (reverse direction) ===
  reset_downstream_test_context();

  // Set up receive callback on upstream
  apps.upstream.set_evt_on_forward_request([](atframework::atapp::app &,
                                              const atframework::atapp::app::message_sender_t &sender,
                                              const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_downstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_downstream_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_downstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "C.2: upstream received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // Send from downstream to upstream
  char msg_data3[] = "upstream-from-downstream-C2";
  gsl::span<const unsigned char> msg_span3{reinterpret_cast<const unsigned char *>(msg_data3),
                                           static_cast<size_t>(strlen(msg_data3))};
  CASE_EXPECT_EQ(0, apps.downstream.send_message(apps.upstream.get_app_id(), 100, msg_span3, &seq));

  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 1; });

  CASE_EXPECT_EQ(1, g_downstream_test_ctx.received_message_count);
  if (!g_downstream_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("upstream-from-downstream-C2", g_downstream_test_ctx.received_messages[0]);
  }
  if (!g_downstream_test_ctx.received_direct_source_ids.empty()) {
    CASE_EXPECT_EQ(apps.downstream.get_app_id(), g_downstream_test_ctx.received_direct_source_ids[0]);
  }
}

// ============================================================
// C.3: downstream_reconnect_before_timeout
// Handle set unready → pending message queued → set_sys_now() advance
// time but BEFORE retry timeout → handle set ready → pending delivered.
// ============================================================
CASE_TEST(atapp_downstream_send, downstream_reconnect_before_timeout) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_handle_unready_by_bus_id() only available in Debug builds, skip"
                  << '\n';
  return;
#else
  reset_downstream_test_context();

  downstream_two_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.wait_for_downstream_connected();
  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();
  apps.pump_until_upstream_can_send();

  // Set up receive callback on downstream
  apps.downstream.set_evt_on_forward_request([](atframework::atapp::app &,
                                                const atframework::atapp::app::message_sender_t &sender,
                                                const atframework::atapp::app::message_t &msg) {
    auto received = gsl::string_view{reinterpret_cast<const char *>(msg.data.data()), msg.data.size()};
    g_downstream_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_downstream_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_downstream_test_ctx.received_message_count;
    CASE_MSG_INFO() << "C.3: downstream received: " << received << ", direct_source=0x" << std::hex
                    << sender.direct_source_id << std::dec << '\n';
    return 0;
  });

  // First message: verify normal send works
  char msg_data1[] = "before-disconnect-C3";
  gsl::span<const unsigned char> msg_span1{reinterpret_cast<const unsigned char *>(msg_data1),
                                           static_cast<size_t>(strlen(msg_data1))};
  uint64_t seq = 0;
  CASE_EXPECT_EQ(0, apps.upstream.send_message(apps.downstream.get_app_id(), 100, msg_span1, &seq));

  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 1; });
  CASE_EXPECT_EQ(1, g_downstream_test_ctx.received_message_count);

  // Simulate disconnect by setting handle unready
  auto up_connector = apps.upstream.get_atbus_connector();
  CASE_EXPECT_TRUE(up_connector != nullptr);
  if (up_connector) {
    CASE_MSG_INFO() << "C.3: Setting handle unready to simulate disconnect..." << '\n';
    up_connector->set_handle_unready_by_bus_id(apps.downstream.get_app_id());
  }

  // Second message: should go to pending queue
  char msg_data2[] = "during-reconnect-C3";
  gsl::span<const unsigned char> msg_span2{reinterpret_cast<const unsigned char *>(msg_data2),
                                           static_cast<size_t>(strlen(msg_data2))};
  apps.upstream.send_message(apps.downstream.get_app_id(), 100, msg_span2, &seq);

  // Verify message is pending
  auto *ep_down = apps.upstream.get_endpoint(apps.downstream.get_app_id());
  CASE_EXPECT_TRUE(ep_down != nullptr);
  if (ep_down != nullptr) {
    CASE_MSG_INFO() << "C.3: pending count during disconnect: " << ep_down->get_pending_message_count() << '\n';
    CASE_EXPECT_GT(ep_down->get_pending_message_count(), static_cast<size_t>(0));
  }

  // Simulate reconnect — set handle back to ready
  if (up_connector) {
    CASE_MSG_INFO() << "C.3: Setting handle ready to simulate reconnect..." << '\n';
    up_connector->set_handle_ready_by_bus_id(apps.downstream.get_app_id());
  }

  // Pump to let pending message be delivered
  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 2; });
  CASE_EXPECT_GE(g_downstream_test_ctx.received_message_count, 2);

  // Third message: after reconnection, should send directly
  char msg_data3[] = "after-reconnect-C3";
  gsl::span<const unsigned char> msg_span3{reinterpret_cast<const unsigned char *>(msg_data3),
                                           static_cast<size_t>(strlen(msg_data3))};
  CASE_EXPECT_EQ(0, apps.upstream.send_message(apps.downstream.get_app_id(), 100, msg_span3, &seq));

  apps.pump_until([&]() { return g_downstream_test_ctx.received_message_count >= 3; });

  CASE_EXPECT_EQ(3, g_downstream_test_ctx.received_message_count);

  // Verify all 3 messages data content
  if (g_downstream_test_ctx.received_messages.size() >= 3) {
    CASE_EXPECT_EQ("before-disconnect-C3", g_downstream_test_ctx.received_messages[0]);
    CASE_EXPECT_EQ("during-reconnect-C3", g_downstream_test_ctx.received_messages[1]);
    CASE_EXPECT_EQ("after-reconnect-C3", g_downstream_test_ctx.received_messages[2]);
  }
  // Verify direct_source_id — should be upstream's app_id
  for (size_t i = 0; i < g_downstream_test_ctx.received_direct_source_ids.size(); ++i) {
    CASE_EXPECT_EQ(apps.upstream.get_app_id(), g_downstream_test_ctx.received_direct_source_ids[i]);
  }
  CASE_MSG_INFO() << "C.3: Total messages received: " << g_downstream_test_ctx.received_message_count << '\n';
#endif
}

// ============================================================
// C.4: downstream_topology_offline_timeout_fail
// Topology removed via remove_topology_peer → kLostTopology flag set →
// handle set unready → advance time past lost_topology_timeout →
// handle force-removed, pending messages fail, GC cleans up endpoint.
// (Merged from previous C.4 + C.5)
// ============================================================
CASE_TEST(atapp_downstream_send, downstream_topology_offline_timeout_fail) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_downstream_test_context();

  downstream_two_node_apps apps;
  if (!apps.init_all()) {
    return;
  }

  apps.wait_for_downstream_connected();
  apps.inject_all_discovery();
  apps.setup_topology();
  apps.create_endpoints();
  apps.pump_until_upstream_can_send();

  // Setup forward response callback to capture error
  apps.upstream.set_evt_on_forward_response([](atframework::atapp::app &,
                                               const atframework::atapp::app::message_sender_t &,
                                               const atframework::atapp::app::message_t &, int32_t error_code) {
    g_downstream_test_ctx.last_forward_response_error = error_code;
    ++g_downstream_test_ctx.forward_response_count;
    CASE_MSG_INFO() << "C.4: forward response error_code=" << error_code << '\n';
    return 0;
  });

  auto up_connector = apps.upstream.get_atbus_connector();
  CASE_EXPECT_TRUE(up_connector != nullptr);

  // Verify handle exists and is ready before topology removal
  if (up_connector) {
    CASE_EXPECT_TRUE(up_connector->has_connection_handle(apps.downstream.get_app_id()));
  }

  // Remove topology for downstream — sets kLostTopology flag
  if (up_connector) {
    // Sync jiffies timer before adding connection timers
    apps.upstream.tick();

    up_connector->remove_topology_peer(apps.downstream.get_app_id());
  }

  // Handle should still exist with kLostTopology flag set
  if (up_connector) {
    CASE_EXPECT_TRUE(up_connector->has_connection_handle(apps.downstream.get_app_id()));
    CASE_EXPECT_TRUE(up_connector->has_lost_topology_flag(apps.downstream.get_app_id()));
    CASE_MSG_INFO() << "C.4: has_lost_topology_flag="
                    << up_connector->has_lost_topology_flag(apps.downstream.get_app_id()) << '\n';
  }

  // Simulate handle becoming unready after topology loss
  if (up_connector) {
    up_connector->set_handle_unready_by_bus_id(apps.downstream.get_app_id());
  }

  // Verify handle is unready
  auto *ep_down = apps.upstream.get_endpoint(apps.downstream.get_app_id());
  CASE_EXPECT_TRUE(ep_down != nullptr);
  if (ep_down != nullptr) {
    CASE_EXPECT_TRUE(ep_down->get_ready_connection_handle() == nullptr);
  }

  // Send message — should go to pending queue
  char msg_data[] = "will-fail-C4";
  gsl::span<const unsigned char> msg_span{reinterpret_cast<const unsigned char *>(msg_data),
                                          static_cast<size_t>(strlen(msg_data))};
  uint64_t seq = 0;
  apps.upstream.send_message(apps.downstream.get_app_id(), 100, msg_span, &seq);

  // Advance time past lost_topology_timeout (32s).
  // Only call tick() to avoid shared uv_default_loop() interference.
  auto now = atframework::atapp::app::get_sys_now();
  bool handle_removed = false;

  for (int i = 1; i <= 1000; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::milliseconds(i * 50));
    apps.upstream.tick();
    if (!handle_removed && up_connector && !up_connector->has_connection_handle(apps.downstream.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "C.4: handle removed after " << i * 50 << "ms" << '\n';
    }
    if (handle_removed && g_downstream_test_ctx.forward_response_count > 0) {
      break;
    }
  }

  // Verify forward response error was triggered
  CASE_MSG_INFO() << "C.4: forward response count=" << g_downstream_test_ctx.forward_response_count << '\n';
  CASE_EXPECT_GT(g_downstream_test_ctx.forward_response_count, 0);
  if (g_downstream_test_ctx.forward_response_count > 0) {
    CASE_EXPECT_EQ(EN_ATBUS_ERR_NODE_TIMEOUT, g_downstream_test_ctx.last_forward_response_error);
  }

  // Handle should be removed
  CASE_EXPECT_TRUE(handle_removed);
  CASE_MSG_INFO() << "C.4: handle_removed=" << handle_removed << '\n';

  // After GC timeout, verify endpoint is cleaned up
  atframework::atapp::app::set_sys_now(now + std::chrono::seconds(200));
  apps.upstream.tick();

  ep_down = apps.upstream.get_endpoint(apps.downstream.get_app_id());
  CASE_EXPECT_TRUE(!ep_down);
  CASE_MSG_INFO() << "C.4: endpoint cleaned up after GC=" << (!ep_down) << '\n';

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}
