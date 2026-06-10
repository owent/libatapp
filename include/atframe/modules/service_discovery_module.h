// Copyright 2026 atframework
// Created by yousongyang

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <atframe/atapp_conf.h>

#include <time/time_utility.h>

#include <atframe/atapp_module_impl.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_discovery.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>
#include <atframe/modules/etcd_module.h>

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

LIBATAPP_MACRO_NAMESPACE_BEGIN
class service_discovery_module : public ::atframework::atapp::module_impl {
 public:
  // ============ 服务发现数据和事件相关定义 ============
  using node_action_t = etcd_discovery_action_t;

  struct LIBATAPP_MACRO_API_HEAD_ONLY node_info_t {
    atapp::protocol::atapp_discovery node_discovery;
    uintptr_t context_addr = 0;
    node_action_t action;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY node_list_t {
    std::list<node_info_t> nodes;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY discovery_watcher_sender_list_t {
    std::reference_wrapper<service_discovery_module> atapp_module;
    std::reference_wrapper<const ::atframework::atapp::etcd_response_header> etcd_header;
    std::reference_wrapper<const ::atframework::atapp::etcd_watcher::response_t> etcd_body;
    std::reference_wrapper<const ::atframework::atapp::etcd_watcher::event_t> event;
    std::reference_wrapper<const node_info_t> node;

    inline discovery_watcher_sender_list_t(service_discovery_module &m,
                                           const ::atframework::atapp::etcd_response_header &h,
                                           const ::atframework::atapp::etcd_watcher::response_t &b,
                                           const ::atframework::atapp::etcd_watcher::event_t &e, const node_info_t &n)
        : atapp_module(std::ref(m)),
          etcd_header(std::cref(h)),
          etcd_body(std::cref(b)),
          event(std::cref(e)),
          node(std::cref(n)) {}
  };

  using discovery_watcher_list_callback_t = std::function<void(discovery_watcher_sender_list_t &)>;

  using discovery_snapshot_event_callback_t = std::function<void(const service_discovery_module &)>;
  using discovery_snapshot_event_callback_list_t = std::list<discovery_snapshot_event_callback_t>;
  using discovery_snapshot_event_callback_handle_t = discovery_snapshot_event_callback_list_t::iterator;

  using node_event_callback_t = std::function<void(node_action_t, const etcd_discovery_node::ptr_t &)>;
  using node_event_callback_list_t = std::list<node_event_callback_t>;
  using node_event_callback_handle_t = node_event_callback_list_t::iterator;

  // ============ 拓扑数据和事件相关定义 ============
  using topology_action_t = etcd_watch_event;
  using atapp_topology_info_ptr_t = atfw::util::memory::strong_rc_ptr<atapp::protocol::atapp_topology_info>;

  struct LIBATAPP_MACRO_API_HEAD_ONLY topology_storage_t {
    atapp_topology_info_ptr_t info;
    uintptr_t context_addr = 0;
    etcd_data_version version;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY topology_info_t {
    topology_storage_t storage;
    topology_action_t action;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY topology_list_t {
    std::list<topology_info_t> topologies;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY topology_watcher_sender_list_t {
    std::reference_wrapper<service_discovery_module> atapp_module;
    std::reference_wrapper<const ::atframework::atapp::etcd_response_header> etcd_header;
    std::reference_wrapper<const ::atframework::atapp::etcd_watcher::response_t> etcd_body;
    std::reference_wrapper<const ::atframework::atapp::etcd_watcher::event_t> event;
    std::reference_wrapper<const topology_info_t> topology;

    inline topology_watcher_sender_list_t(service_discovery_module &m,
                                          const ::atframework::atapp::etcd_response_header &h,
                                          const ::atframework::atapp::etcd_watcher::response_t &b,
                                          const ::atframework::atapp::etcd_watcher::event_t &e,
                                          const topology_info_t &n)
        : atapp_module(std::ref(m)),
          etcd_header(std::cref(h)),
          etcd_body(std::cref(b)),
          event(std::cref(e)),
          topology(std::cref(n)) {}
  };

  using topology_watcher_list_callback_t = std::function<void(topology_watcher_sender_list_t &)>;

  using topology_snapshot_event_callback_t = std::function<void(const service_discovery_module &)>;
  using topology_snapshot_event_callback_list_t = std::list<topology_snapshot_event_callback_t>;
  using topology_snapshot_event_callback_handle_t = topology_snapshot_event_callback_list_t::iterator;

  using topology_info_event_callback_t =
      std::function<void(topology_action_t, const atapp_topology_info_ptr_t &, const etcd_data_version &)>;
  using topology_info_event_callback_list_t = std::list<topology_info_event_callback_t>;
  using topology_info_event_callback_handle_t = topology_info_event_callback_list_t::iterator;

  struct service_discovery_cluster_context_data;

  class service_discovery_cluster_context {
   public:
    friend class service_discovery_module;

    LIBATAPP_MACRO_API service_discovery_cluster_context();
    LIBATAPP_MACRO_API int init(atframework::atapp::app &app, const atapp::protocol::atapp_etcd &conf,
                                const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf);
    LIBATAPP_MACRO_API int reload(const atapp::protocol::atapp_etcd &conf,
                                  const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf);
    LIBATAPP_MACRO_API int stop();
    LIBATAPP_MACRO_API void tick();
    LIBATAPP_MACRO_API void reset();

   public:
    LIBATAPP_MACRO_API etcd_module &get_etcd_module();
    LIBATAPP_MACRO_API const etcd_module &get_etcd_module() const;

    LIBATAPP_MACRO_API bool check_keepalive_actor_start_success();
    LIBATAPP_MACRO_API void add_discovery_watcher_by_id_callback(const discovery_watcher_list_callback_t &fn);
    LIBATAPP_MACRO_API void add_discovery_watcher_by_name_callback(const discovery_watcher_list_callback_t &fn);
    LIBATAPP_MACRO_API void add_topology_watcher_callback(const topology_watcher_list_callback_t &fn);

    LIBATAPP_MACRO_API void reset_internal_watchers_and_keepalives();

   private:
    etcd_module etcd_module_;
    ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context_data>> data_;
  };

 public:
  LIBATAPP_MACRO_API service_discovery_module();

 private:
  void update_keepalive_topology_value();
  void update_keepalive_discovery_value();

 public:
  LIBATAPP_MACRO_API int init() override;
  LIBATAPP_MACRO_API const char *ATFW_UTIL_MACRO_NONNULL name() const override;
  LIBATAPP_MACRO_API int stop() override;
  LIBATAPP_MACRO_API int reload() override;
  LIBATAPP_MACRO_API int tick() override;
  LIBATAPP_MACRO_API int timeout() override;

 public:
  LIBATAPP_MACRO_API bool is_discovery_enabled() const;
  LIBATAPP_MACRO_API void enable_discovery();
  LIBATAPP_MACRO_API void disable_discovery();

  LIBATAPP_MACRO_API void set_maybe_update_keepalive_topology_value();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_discovery_value();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_discovery_area();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_discovery_metadata();

  LIBATAPP_MACRO_API static std::string get_discovery_by_id_path(atframework::atapp::app &app, const std::string &path);
  LIBATAPP_MACRO_API static std::string get_discovery_by_name_path(atframework::atapp::app &app,
                                                                   const std::string &path);
  LIBATAPP_MACRO_API static std::string get_topology_path(atframework::atapp::app &app, const std::string &path);

  LIBATAPP_MACRO_API static std::string get_discovery_by_id_watcher_path(atframework::atapp::app &app,
                                                                         const std::string &path);
  LIBATAPP_MACRO_API static std::string get_discovery_by_name_watcher_path(atframework::atapp::app &app,
                                                                           const std::string &path);
  LIBATAPP_MACRO_API static std::string get_topology_watcher_path(atframework::atapp::app &app,
                                                                  const std::string &path);

  LIBATAPP_MACRO_API void reset_context_internal_watchers_and_keepalives();
  LIBATAPP_MACRO_API int init_service_discovery_keepalives(
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);
  LIBATAPP_MACRO_API int init_service_discovery_watchers(
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);

  LIBATAPP_MACRO_API static int init_discovery_watcher_by_id(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);
  LIBATAPP_MACRO_API static int init_discovery_watcher_by_name(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);
  LIBATAPP_MACRO_API static int init_topology_watcher(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);
  LIBATAPP_MACRO_API static int init_discovery_keepalive_by_id(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      std::string &value);
  LIBATAPP_MACRO_API static int init_discovery_keepalive_by_name(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      std::string &value);
  LIBATAPP_MACRO_API static int init_topology_keepalive(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      std::string &value);

  LIBATAPP_MACRO_API static int add_discovery_watcher_by_id_callback(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      const discovery_watcher_list_callback_t &fn);
  LIBATAPP_MACRO_API static int add_discovery_watcher_by_name_callback(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      const discovery_watcher_list_callback_t &fn);
  LIBATAPP_MACRO_API static int add_topology_watcher_callback(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      const topology_watcher_list_callback_t &fn);

  LIBATAPP_MACRO_API const ::atframework::atapp::etcd_cluster &get_raw_etcd_ctx() const;
  LIBATAPP_MACRO_API ::atframework::atapp::etcd_cluster &get_raw_etcd_ctx();

  LIBATAPP_MACRO_API const ::atframework::atapp::etcd_response_header &get_last_etcd_event_topology_header()
      const noexcept;
  LIBATAPP_MACRO_API const ::atframework::atapp::etcd_response_header &get_last_etcd_event_discovery_header()
      const noexcept;

  LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &get_configure() const;
  LIBATAPP_MACRO_API const std::string &get_configure_path();

  LIBATAPP_MACRO_API static atapp::etcd_keepalive::ptr_t add_keepalive_actor(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context, std::string &val,
      const std::string &node_path);
  LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t add_keepalive_actor(atframework::atapp::app &app, std::string &val,
                                                                      const std::string &node_path);

  LIBATAPP_MACRO_API static bool remove_keepalive_actor(
      atframework::atapp::app &app,
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context,
      const atapp::etcd_keepalive::ptr_t &keepalive);
  LIBATAPP_MACRO_API bool remove_keepalive_actor(atframework::atapp::app &app,
                                                 const atapp::etcd_keepalive::ptr_t &keepalive);

  LIBATAPP_MACRO_API node_event_callback_handle_t add_on_node_discovery_event(const node_event_callback_t &fn);
  LIBATAPP_MACRO_API void remove_on_node_event(node_event_callback_handle_t &handle);

  LIBATAPP_MACRO_API topology_info_event_callback_handle_t
  add_on_topology_info_event(const topology_info_event_callback_t &fn);
  LIBATAPP_MACRO_API void remove_on_topology_info_event(topology_info_event_callback_handle_t &handle);

  LIBATAPP_MACRO_API etcd_discovery_set &get_global_discovery() noexcept;
  LIBATAPP_MACRO_API const etcd_discovery_set &get_global_discovery() const noexcept;

  LIBATAPP_MACRO_API const std::unordered_map<uint64_t, topology_storage_t> &get_topology_info_set() const noexcept;

  LIBATAPP_MACRO_API bool has_discovery_snapshot() const noexcept;

  LIBATAPP_MACRO_API discovery_snapshot_event_callback_handle_t
  add_on_load_discovery_snapshot(const discovery_snapshot_event_callback_t &fn);
  LIBATAPP_MACRO_API void remove_on_load_discovery_snapshot(discovery_snapshot_event_callback_handle_t &handle);
  LIBATAPP_MACRO_API discovery_snapshot_event_callback_handle_t
  add_on_discovery_snapshot_loaded(const discovery_snapshot_event_callback_t &fn);
  LIBATAPP_MACRO_API void remove_on_discovery_snapshot_loaded(discovery_snapshot_event_callback_handle_t &handle);

  LIBATAPP_MACRO_API bool has_topology_snapshot() const noexcept;

  LIBATAPP_MACRO_API topology_snapshot_event_callback_handle_t
  add_on_load_topology_snapshot(const topology_snapshot_event_callback_t &fn);
  LIBATAPP_MACRO_API void remove_on_load_topology_snapshot(topology_snapshot_event_callback_handle_t &handle);
  LIBATAPP_MACRO_API topology_snapshot_event_callback_handle_t
  add_on_topology_snapshot_loaded(const topology_snapshot_event_callback_t &fn);
  LIBATAPP_MACRO_API void remove_on_topology_snapshot_loaded(topology_snapshot_event_callback_handle_t &handle);

  LIBATAPP_MACRO_API etcd_watcher::watch_event_fn_t_ptr create_discovery_watcher_callback_list_wrapper(
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);
  LIBATAPP_MACRO_API etcd_watcher::watch_event_fn_t_ptr create_topology_watcher_callback_list_wrapper(
      const ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> &context);

 private:
  static bool unpack(node_info_t &out, const std::string &path, const std::string &json, bool reset_data);
  static void pack(const node_info_t &src, std::string &json);
  static bool unpack(topology_info_t &out, const std::string &path, const std::string &json, bool reset_data);
  static void pack(const atapp::protocol::atapp_topology_info &src, std::string &json);

  struct topology_watcher_callback_list_wrapper_t;
  struct discovery_watcher_callback_list_wrapper_t;

  bool update_internal_watcher_event(node_info_t &node, const etcd_discovery_node::node_version &version);
  bool update_internal_watcher_event(topology_info_t &topology_info);

  struct watcher_internal_access_t;

 private:
  bool discovery_enabled_;
  ::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>> cluster_context_;
  std::list<::atfw::util::nostd::nonnull<std::shared_ptr<service_discovery_cluster_context>>>
      external_cluster_contexts_;

  bool maybe_update_internal_keepalive_topology_value_;
  bool maybe_update_internal_keepalive_discovery_value_;
  bool maybe_update_internal_keepalive_discovery_area_;
  bool maybe_update_internal_keepalive_discovery_metadata_;

  atapp_topology_info_ptr_t last_submitted_topology_data_;
  atapp::protocol::atapp_area last_submitted_discovery_data_area_;
  atapp::protocol::atapp_metadata last_submitted_discovery_data_metadata_;

  std::string internal_keepalive_topology_value_;
  std::string internal_keepalive_discovery_value_;

  discovery_snapshot_event_callback_list_t discovery_on_load_snapshot_callbacks_;
  discovery_snapshot_event_callback_list_t discovery_on_snapshot_loaded_callbacks_;

  topology_snapshot_event_callback_list_t topology_on_load_snapshot_callbacks_;
  topology_snapshot_event_callback_list_t topology_on_snapshot_loaded_callbacks_;

  etcd_discovery_set global_discovery_;
  std::unordered_map<uint64_t, topology_storage_t> internal_topology_info_set_;

  mutable std::recursive_mutex node_event_lock_;
  node_event_callback_list_t node_event_callbacks_;

  mutable std::recursive_mutex topology_info_event_lock_;
  topology_info_event_callback_list_t topology_info_event_callbacks_;
};
LIBATAPP_MACRO_NAMESPACE_END
