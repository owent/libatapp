// Copyright 2026 atframework
// Test E: Service discovery lifecycle & reconnect mechanism tests
//
// Topology: node1(0x501) --proxy--> upstream(0x503) <--proxy-- node2(0x502)
// allow_direct_connection: true → node1 and node2 are kSameUpstreamPeer
//
// E.1: on_discovery_event(kDelete) → kWaitForDiscoveryToConnect → kPut → reconnect
// E.2: discovery missing → reconnect_retry_times accumulates → max exceeded → handle removed
// E.3: reconnect exponential backoff 2s→4s→8s→16s→16s
// E.4: update_timer replaces timer when new timeout < old
// E.5: update_timer skips when new timeout > old

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

// Mirror of atbus_connection_handle_flags_t from atapp_connector_atbus.cpp (internal enum)
static constexpr uint32_t kAtbusHandleFlagWaitForDiscoveryToConnect = 0x02;

struct discovery_test_context {
  int received_message_count = 0;
  std::vector<std::string> received_messages;
  std::vector<atframework::atapp::app_id_t> received_direct_source_ids;
};

static discovery_test_context g_disc_test_ctx;

static void reset_disc_test_context() {
  g_disc_test_ctx.received_message_count = 0;
  g_disc_test_ctx.received_messages.clear();
  g_disc_test_ctx.received_direct_source_ids.clear();
}

static std::string get_disc_test_conf_path(const char *filename) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/" + filename;
}

static bool check_disc_and_skip_if_missing(const std::string &path) {
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
    g_disc_test_ctx.received_messages.emplace_back(received.data(), received.size());
    g_disc_test_ctx.received_direct_source_ids.push_back(sender.direct_source_id);
    ++g_disc_test_ctx.received_message_count;
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

// Setup 3-node topology and discovery for the E group
static void setup_discovery_test_env(atframework::atapp::app &node1, atframework::atapp::app &node2,
                                     atframework::atapp::app &upstream,
                                     atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &n1_disc,
                                     atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &n2_disc,
                                     atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> &up_disc) {
  // Update bus topology registries
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

  // Update connector topology
  auto up_conn = upstream.get_atbus_connector();
  if (up_conn) {
    up_conn->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
    up_conn->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
  }
  auto n1_conn = node1.get_atbus_connector();
  if (n1_conn) {
    n1_conn->update_topology_peer(upstream.get_app_id(), 0, nullptr);
    n1_conn->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);
  }
  auto n2_conn = node2.get_atbus_connector();
  if (n2_conn) {
    n2_conn->update_topology_peer(upstream.get_app_id(), 0, nullptr);
    n2_conn->update_topology_peer(node1.get_app_id(), upstream.get_app_id(), nullptr);
  }

  // Create discovery nodes
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

  // Create endpoints for all pairs
  CASE_EXPECT_TRUE(node1.mutable_endpoint(n2_disc));
  CASE_EXPECT_TRUE(node1.mutable_endpoint(up_disc));
  CASE_EXPECT_TRUE(node2.mutable_endpoint(n1_disc));
  CASE_EXPECT_TRUE(node2.mutable_endpoint(up_disc));
  CASE_EXPECT_TRUE(upstream.mutable_endpoint(n1_disc));
  CASE_EXPECT_TRUE(upstream.mutable_endpoint(n2_disc));
}

}  // namespace

// ============================================================
// E.1: discovery_delete_then_put_reconnect
// on_discovery_event(kDelete) → kWaitForDiscoveryToConnect set →
// on_discovery_event(kPut) → resume_handle_discovery → reconnect OK.
// ============================================================
CASE_TEST(atapp_discovery_reconnect, discovery_delete_then_put_reconnect) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW)
                  << "get_connection_handle_debug_info() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_disc_test_context();

  std::string conf1 = get_disc_test_conf_path("atapp_test_discovery_1.yaml");
  std::string conf2 = get_disc_test_conf_path("atapp_test_discovery_2.yaml");
  std::string conf3 = get_disc_test_conf_path("atapp_test_discovery_3.yaml");

  if (check_disc_and_skip_if_missing(conf1)) return;
  if (check_disc_and_skip_if_missing(conf2)) return;
  if (check_disc_and_skip_if_missing(conf3)) return;

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

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, n2_disc, up_disc;
  setup_discovery_test_env(node1, node2, upstream, n1_disc, n2_disc, up_disc);

  // Pump until node1→node2 handle is ready (direct connection, allow_direct_connection=true)
  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  // Verify initial state: handle ready, message works
  setup_receive_callback(node2, "E.1");
  CASE_EXPECT_EQ(0, send_test_message(node1, node2.get_app_id(), "before-delete-E1"));
  pump_apps_until([&]() { return g_disc_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), upstream,
                  node1, node2);
  CASE_EXPECT_EQ(1, g_disc_test_ctx.received_message_count);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  // Verify handle is ready before topology change
  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  // Verify kWaitForDiscoveryToConnect is NOT set in the initial state
  auto dbg_initial = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg_initial.exists);
  CASE_EXPECT_EQ(0, dbg_initial.flags & kAtbusHandleFlagWaitForDiscoveryToConnect);
  CASE_MSG_INFO() << "E.1: initial flags=0x" << std::hex << dbg_initial.flags << std::dec << '\n';

  // ---- Phase 2: Trigger kWaitForDiscoveryToConnect ----
  // Step 1: Remove the direct bus endpoint for node2 (on_remove_endpoint fires synchronously,
  //         re-binds proxy via try_direct_reconnect but does NOT create a new bus endpoint yet)
  auto bus1 = node1.get_bus_node();
  CASE_EXPECT_TRUE(bus1 != nullptr);
  if (bus1) {
    bus1->remove_endpoint(node2.get_app_id());
  }

  // Step 2: Remove node2's discovery so update_topology_peer can't find it
  node1.get_etcd_module()->get_global_discovery().remove_node(n2_disc);

  // Step 3: update_topology_peer(upstream=0) unbinds the re-bound proxy (proxy_bus_id→0),
  // then: no bus endpoint, no discovery → set_handle_waiting_discovery → kWaitForDiscoveryToConnect SET
  n1_conn->update_topology_peer(node2.get_app_id(), 0, nullptr);

  // Verify kWaitForDiscoveryToConnect IS set
  auto dbg_after_waiting = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg_after_waiting.exists);
  CASE_EXPECT_NE(0, dbg_after_waiting.flags & kAtbusHandleFlagWaitForDiscoveryToConnect);
  CASE_MSG_INFO() << "E.1: flags after waiting=0x" << std::hex << dbg_after_waiting.flags << std::dec << '\n';

  // ---- Phase 3: Resume via kPut event ----
  // Re-add discovery and fire kPut → resume_handle_discovery clears flag and reconnects
  node1.get_etcd_module()->get_global_discovery().add_node(n2_disc);
  n1_conn->on_discovery_event(atframework::atapp::etcd_discovery_action_t::kPut, n2_disc);

  // Verify kWaitForDiscoveryToConnect IS cleared after kPut
  auto dbg_after_put = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg_after_put.exists);
  CASE_EXPECT_EQ(0, dbg_after_put.flags & kAtbusHandleFlagWaitForDiscoveryToConnect);
  CASE_MSG_INFO() << "E.1: flags after put=0x" << std::hex << dbg_after_put.flags << std::dec << '\n';

  // Restore the correct topology so the connector can find the proxy path for reconnection
  n1_conn->update_topology_peer(node2.get_app_id(), upstream.get_app_id(), nullptr);

  // Pump to let reconnection establish
  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  // Verify handle recovered: send message again
  reset_disc_test_context();
  setup_receive_callback(node2, "E.1");
  CASE_EXPECT_EQ(0, send_test_message(node1, node2.get_app_id(), "after-put-E1"));
  pump_apps_until([&]() { return g_disc_test_ctx.received_message_count >= 1; }, std::chrono::seconds(8), upstream,
                  node1, node2);
  CASE_EXPECT_EQ(1, g_disc_test_ctx.received_message_count);
  if (!g_disc_test_ctx.received_messages.empty()) {
    CASE_EXPECT_EQ("after-put-E1", g_disc_test_ctx.received_messages[0]);
  }
#endif
}

// ============================================================
// E.2: discovery_missing_reconnect_count_accumulate
// Discovery missing → set_sys_now to trigger timer → reconnect_retry_times
// increments → exceeds max_try_times → handle removed.
// ============================================================
CASE_TEST(atapp_discovery_reconnect, discovery_missing_reconnect_count_accumulate) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_disc_test_context();

  std::string conf1 = get_disc_test_conf_path("atapp_test_discovery_1.yaml");
  std::string conf2 = get_disc_test_conf_path("atapp_test_discovery_2.yaml");
  std::string conf3 = get_disc_test_conf_path("atapp_test_discovery_3.yaml");

  if (check_disc_and_skip_if_missing(conf1)) return;
  if (check_disc_and_skip_if_missing(conf2)) return;
  if (check_disc_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app node2;
  atframework::atapp::app upstream;

  const char *args3[] = {"upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, upstream.init(nullptr, 4, args3, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args2[] = {"node2", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, node2.init(nullptr, 4, args2, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b2 = node2.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b2 && b2->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, n2_disc, up_disc;
  setup_discovery_test_env(node1, node2, upstream, n1_disc, n2_disc, up_disc);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  // Remove node2 discovery from node1 so reconnect can't find it
  node1.get_etcd_module()->get_global_discovery().remove_node(n2_disc);

  // Force handle into non-ready state using debug setter — this also starts reconnect timer
  // (set_handle_unready_by_bus_id calls set_handle_unready + setup_reconnect_timer)
  n1_conn->set_handle_unready_by_bus_id(node2.get_app_id());
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  auto dbg0 = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg0.exists);
  // set_handle_unready_by_bus_id calls setup_reconnect_timer which increments retry_times from 0→1
  CASE_MSG_INFO() << "E.2: initial retry_times=" << dbg0.reconnect_retry_times << '\n';

  // Sync jiffies timer
  node1.tick();

  // Config: reconnect_max_try_times=3, reconnect_start_interval=2s, reconnect_max_interval=16s
  // setup_reconnect_timer incremented retry_times to 1, set reconnect timer at now+2s
  // Timer at 2s: retry_times→2, interval=4s, next at 6s
  // Timer at 6s: retry_times→3 ≥ max=3 → handle removed
  auto now = atframework::atapp::app::get_sys_now();
  bool handle_removed = false;

  for (int i = 1; i <= 100; ++i) {
    atframework::atapp::app::set_sys_now(now + std::chrono::seconds(i));
    node1.tick();
    if (!n1_conn->has_connection_handle(node2.get_app_id())) {
      handle_removed = true;
      CASE_MSG_INFO() << "E.2: handle removed after " << i << "s" << '\n';
      break;
    }
  }

  CASE_EXPECT_TRUE(handle_removed);

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// E.3: reconnect_exponential_backoff
// Config: start_interval=2s, max_interval=8s, max_try_times=8
// Verify: intervals double 2s→4s→8s then cap at max_interval
// ============================================================
CASE_TEST(atapp_discovery_reconnect, reconnect_exponential_backoff) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_disc_test_context();

  // Use discovery_1b.yaml: reconnect_max_interval=8s, reconnect_max_try_times=8
  std::string conf1 = get_disc_test_conf_path("atapp_test_discovery_1b.yaml");
  std::string conf2 = get_disc_test_conf_path("atapp_test_discovery_2.yaml");
  std::string conf3 = get_disc_test_conf_path("atapp_test_discovery_3.yaml");

  if (check_disc_and_skip_if_missing(conf1)) return;
  if (check_disc_and_skip_if_missing(conf2)) return;
  if (check_disc_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app node2;
  atframework::atapp::app upstream;

  const char *args3[] = {"upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, upstream.init(nullptr, 4, args3, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args2[] = {"node2", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, node2.init(nullptr, 4, args2, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b2 = node2.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b2 && b2->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, n2_disc, up_disc;
  setup_discovery_test_env(node1, node2, upstream, n1_disc, n2_disc, up_disc);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  CASE_EXPECT_TRUE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  // Remove node2 discovery so reconnects will fail
  node1.get_etcd_module()->get_global_discovery().remove_node(n2_disc);

  // Capture now BEFORE set_handle_unready_by_bus_id
  auto now = atframework::atapp::app::get_sys_now();
  auto epoch = std::chrono::system_clock::from_time_t(0);

  // Force handle into non-ready state — starts reconnect timer
  n1_conn->set_handle_unready_by_bus_id(node2.get_app_id());
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(node2.get_app_id()));
  node1.tick();

  // Config: start_interval=2s, max_interval=8s, max_try_times=8
  // Backoff: interval = min(start_interval * 2^retry_count, max_interval)
  // retry 0→1: 2s, retry 1→2: 4s, retry 2→3: 8s (=max), retry 3→4: 8s (capped), ...
  auto dbg = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg.exists);
  auto rnt = dbg.reconnect_next_timepoint;
  CASE_EXPECT_NE(epoch, rnt);

  // Verify initial interval: start_interval = 2s
  auto initial_interval = std::chrono::duration_cast<std::chrono::seconds>(rnt - now);
  CASE_EXPECT_EQ(2, initial_interval.count());
  CASE_MSG_INFO() << "E.3: initial interval=" << initial_interval.count()
                  << "s, retry_times=" << dbg.reconnect_retry_times << '\n';

  // Advance through each reconnect timepoint and collect intervals
  std::vector<int64_t> intervals;
  const int max_rounds = 10;

  for (int round = 0; round < max_rounds; ++round) {
    auto prev_rnt = rnt;

    // Advance to just past the reconnect timepoint
    atframework::atapp::app::set_sys_now(prev_rnt + std::chrono::milliseconds(100));
    node1.tick();

    if (!n1_conn->has_connection_handle(node2.get_app_id())) {
      CASE_MSG_INFO() << "E.3: handle removed at round " << round << '\n';
      break;
    }

    dbg = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
    if (!dbg.exists || dbg.reconnect_next_timepoint == epoch) break;
    rnt = dbg.reconnect_next_timepoint;

    // interval = rnt_new - prev_rnt ≈ actual interval (±100ms from callback timing)
    auto interval = std::chrono::duration_cast<std::chrono::seconds>(rnt - prev_rnt);
    intervals.push_back(interval.count());
    CASE_MSG_INFO() << "E.3: round " << round << " interval=" << interval.count()
                    << "s, retry_times=" << dbg.reconnect_retry_times << '\n';
  }

  // Need at least 4 intervals: 4s, 8s, 8s, 8s to verify cap
  CASE_EXPECT_GE(intervals.size(), 4u);

  // Verify exponential increase
  if (intervals.size() >= 1) {
    CASE_EXPECT_EQ(4, intervals[0]);  // 2s * 2^1 = 4s
  }
  if (intervals.size() >= 2) {
    CASE_EXPECT_EQ(8, intervals[1]);  // 2s * 2^2 = 8s = max_interval
  }
  // Verify cap at max_interval: subsequent intervals must equal max_interval
  if (intervals.size() >= 3) {
    CASE_EXPECT_EQ(8, intervals[2]);  // capped at max_interval
  }
  if (intervals.size() >= 4) {
    CASE_EXPECT_EQ(8, intervals[3]);  // still capped at max_interval
  }

  // Verify cap is maintained: no increase beyond max_interval
  if (intervals.size() >= 3) {
    CASE_EXPECT_EQ(intervals[1], intervals[2]);
    CASE_MSG_INFO() << "E.3: confirmed cap at max_interval=" << intervals[1] << "s" << '\n';
  }

  // Handle should have been removed (max_try_times=8)
  CASE_EXPECT_FALSE(n1_conn->has_connection_handle(node2.get_app_id()));

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// E.4: reconnect_timer_replaced_by_earlier
// Config: start_interval=30s, lost_topology_timeout=8s
// set_handle_unready_by_bus_id → FAR timer (30s)
// remove_topology_peer → update_timer(8s) → REPLACEMENT (8s < 30s)
// Verify pending_timer_timeout decreases.
// ============================================================
CASE_TEST(atapp_discovery_reconnect, reconnect_timer_replaced_by_earlier) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_disc_test_context();

  // Use discovery_4.yaml: reconnect_start_interval=30s, lost_topology_timeout=8s
  std::string conf1 = get_disc_test_conf_path("atapp_test_discovery_4.yaml");
  std::string conf2 = get_disc_test_conf_path("atapp_test_discovery_2.yaml");
  std::string conf3 = get_disc_test_conf_path("atapp_test_discovery_3.yaml");

  if (check_disc_and_skip_if_missing(conf1)) return;
  if (check_disc_and_skip_if_missing(conf2)) return;
  if (check_disc_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app node2;
  atframework::atapp::app upstream;

  const char *args3[] = {"upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, upstream.init(nullptr, 4, args3, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args2[] = {"node2", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, node2.init(nullptr, 4, args2, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b2 = node2.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b2 && b2->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, n2_disc, up_disc;
  setup_discovery_test_env(node1, node2, upstream, n1_disc, n2_disc, up_disc);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  auto epoch = std::chrono::system_clock::from_time_t(0);

  // Step 1: set_handle_unready_by_bus_id → clears kReady, starts FAR reconnect timer (30s)
  n1_conn->set_handle_unready_by_bus_id(node2.get_app_id());
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(node2.get_app_id()));
  node1.tick();

  // Record the FAR pending_timer_timeout
  auto dbg_far = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg_far.exists);
  auto far_timeout = dbg_far.pending_timer_timeout;
  CASE_EXPECT_NE(epoch, far_timeout);
  CASE_MSG_INFO() << "E.4: FAR timer set (start_interval=30s), retry_times=" << dbg_far.reconnect_retry_times << '\n';

  // Step 2: remove_topology_peer → kLostTopology set, update_timer(now+8s)
  // Since 8s < 30s → existing FAR timer is REPLACED by NEAR timer
  n1_conn->remove_topology_peer(node2.get_app_id());
  CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(node2.get_app_id()));

  // Record the NEAR pending_timer_timeout
  auto dbg_near = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg_near.exists);
  auto near_timeout = dbg_near.pending_timer_timeout;
  CASE_EXPECT_NE(epoch, near_timeout);

  // ---- Core assertion: pending_timer_timeout DECREASED ----
  CASE_EXPECT_LT(near_timeout, far_timeout);
  CASE_MSG_INFO() << "E.4: NEAR timer replaced FAR timer ("
                  << std::chrono::duration_cast<std::chrono::seconds>(near_timeout - far_timeout).count()
                  << "s earlier)" << '\n';

  // Verify the near timeout is approximately lost_topology_timeout (8s) from now
  auto now = atframework::atapp::app::get_sys_now();
  auto near_from_now = std::chrono::duration_cast<std::chrono::seconds>(near_timeout - now);
  CASE_EXPECT_GE(near_from_now.count(), 7);  // approximately 8s (±1s jitter)
  CASE_EXPECT_LE(near_from_now.count(), 9);
  CASE_MSG_INFO() << "E.4: near_timeout is +" << near_from_now.count() << "s from now" << '\n';

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}

// ============================================================
// E.5: reconnect_timer_skip_if_later
// update_timer: if new timeout >= old timeout → timer unchanged.
// Force non-ready, then set a near reconnect timer, then
// remove_topology_peer which sets a far lost_topology timer.
// Verify pending_timer_timeout unchanged (kept at near value).
// ============================================================
CASE_TEST(atapp_discovery_reconnect, reconnect_timer_skip_if_later) {
#if defined(NDEBUG)
  CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << "set_sys_now() only available in Debug builds, skip" << '\n';
  return;
#else
  reset_disc_test_context();

  std::string conf1 = get_disc_test_conf_path("atapp_test_discovery_1.yaml");
  std::string conf2 = get_disc_test_conf_path("atapp_test_discovery_2.yaml");
  std::string conf3 = get_disc_test_conf_path("atapp_test_discovery_3.yaml");

  if (check_disc_and_skip_if_missing(conf1)) return;
  if (check_disc_and_skip_if_missing(conf2)) return;
  if (check_disc_and_skip_if_missing(conf3)) return;

  atframework::atapp::app node1;
  atframework::atapp::app node2;
  atframework::atapp::app upstream;

  const char *args3[] = {"upstream", "-c", conf3.c_str(), "start"};
  CASE_EXPECT_EQ(0, upstream.init(nullptr, 4, args3, nullptr));
  const char *args1[] = {"node1", "-c", conf1.c_str(), "start"};
  CASE_EXPECT_EQ(0, node1.init(nullptr, 4, args1, nullptr));
  const char *args2[] = {"node2", "-c", conf2.c_str(), "start"};
  CASE_EXPECT_EQ(0, node2.init(nullptr, 4, args2, nullptr));

  pump_apps_until(
      [&]() {
        auto b1 = node1.get_bus_node();
        auto b2 = node2.get_bus_node();
        return b1 && b1->get_upstream_endpoint() != nullptr && b2 && b2->get_upstream_endpoint() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  atfw::util::memory::strong_rc_ptr<atapp::etcd_discovery_node> n1_disc, n2_disc, up_disc;
  setup_discovery_test_env(node1, node2, upstream, n1_disc, n2_disc, up_disc);

  pump_apps_until(
      [&]() {
        auto *ep = node1.get_endpoint(node2.get_app_id());
        return ep != nullptr && ep->get_ready_connection_handle() != nullptr;
      },
      std::chrono::seconds(8), upstream, node1, node2);

  auto n1_conn = node1.get_atbus_connector();
  CASE_EXPECT_TRUE(n1_conn != nullptr);
  if (!n1_conn) return;

  // Force handle into non-ready state — starts reconnect timer at now+2s
  n1_conn->set_handle_unready_by_bus_id(node2.get_app_id());
  CASE_EXPECT_FALSE(n1_conn->is_connection_handle_ready(node2.get_app_id()));

  node1.tick();

  // Record the near reconnect timer (now+2s)
  auto dbg1 = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg1.exists);
  auto original_timeout = dbg1.pending_timer_timeout;
  CASE_EXPECT_TRUE(original_timeout != std::chrono::system_clock::from_time_t(0));
  CASE_EXPECT_FALSE(dbg1.timer_handle_expired);
  CASE_MSG_INFO() << "E.5: near reconnect timer set, timer_expired=" << dbg1.timer_handle_expired << '\n';

  // Now remove_topology_peer sets kLostTopology and calls update_timer(now+32s)
  // Since existing timer (now+2s) is EARLIER than new timer (now+32s), update_timer SKIPS
  n1_conn->remove_topology_peer(node2.get_app_id());
  CASE_EXPECT_TRUE(n1_conn->has_lost_topology_flag(node2.get_app_id()));

  node1.tick();

  auto dbg2 = n1_conn->get_connection_handle_debug_info(node2.get_app_id());
  CASE_EXPECT_TRUE(dbg2.exists);

  // The timer should remain at the original (earlier) timeout
  if (original_timeout != std::chrono::system_clock::from_time_t(0) &&
      dbg2.pending_timer_timeout != std::chrono::system_clock::from_time_t(0)) {
    CASE_EXPECT_EQ(original_timeout, dbg2.pending_timer_timeout);
    CASE_MSG_INFO() << "E.5: timer unchanged (skip later timeout)" << '\n';
  }

  // Restore system time
  atframework::atapp::app::set_sys_now(atfw::util::time::time_utility::sys_now());
#endif
}
