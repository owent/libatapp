// Copyright 2026 atframework
// Created by yousongyang

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <config/compiler/template_prefix.h>

#include <rapidjson/document.h>

#include <config/compiler/template_suffix.h>

#include <atframe/atapp_conf.h>

#include <network/http_request.h>
#include <random/random_generator.h>
#include <time/time_utility.h>

#include <atframe/atapp_module_impl.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_discovery.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>
#include <atframe/modules/etcd_module.h>

#include <list>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

LIBATAPP_MACRO_NAMESPACE_BEGIN
class service_discovery_module : public ::atframework::atapp::module_impl {
 public:
  // ============ 服务发现数据和事件相关定义 ============
  using node_action_t = etcd_discovery_action_t;

  struct LIBATAPP_MACRO_API_HEAD_ONLY node_info_t {
    atapp::protocol::atapp_discovery node_discovery;
    uintptr_t context_ptr;
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
    uintptr_t context_ptr;
    etcd_data_version version;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY topology_info_t {
    topology_storage_t storage;
    uintptr_t context_ptr;
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

  struct service_discovery_cluster_context {
    LIBATAPP_MACRO_API service_discovery_cluster_context();

    LIBATAPP_MACRO_API void add_discovery_watcher_by_id_callback(const discovery_watcher_list_callback_t &fn);
    LIBATAPP_MACRO_API void add_discovery_watcher_by_name_callback(const discovery_watcher_list_callback_t &fn);
    LIBATAPP_MACRO_API void add_topology_watcher_callback(const topology_watcher_list_callback_t &fn);
    LIBATAPP_MACRO_API void reset_internal_watchers_and_keepalives();

    etcd_cluster_holder_ptr_t etcd_cluster_holder_;

    ::atframework::atapp::etcd_response_header last_etcd_event_topology_header_;
    ::atframework::atapp::etcd_response_header last_etcd_event_discovery_header_;

    std::set<int64_t> discovery_watcher_snapshot_index_;
    int64_t discovery_watcher_snapshot_index_allocator_;
    std::set<int64_t> topology_watcher_snapshot_index_;
    int64_t topology_watcher_snapshot_index_allocator_;
    etcd_watcher::ptr_t internal_topology_watcher_;
    etcd_watcher::ptr_t internal_discovery_watcher_by_name_;
    etcd_watcher::ptr_t internal_discovery_watcher_by_id_;

    std::list<etcd_keepalive::ptr_t> internal_topology_keepalive_actors_;
    std::list<etcd_keepalive::ptr_t> internal_discovery_keepalive_actors_;

    std::list<discovery_watcher_list_callback_t> discovery_watcher_by_id_callbacks_;
    std::list<discovery_watcher_list_callback_t> discovery_watcher_by_name_callbacks_;
    std::list<topology_watcher_list_callback_t> topology_watcher_callbacks_;

    mutable std::recursive_mutex discovery_watcher_callback_lock_;
    mutable std::recursive_mutex topology_watcher_callback_lock_;
  };

 public:
  LIBATAPP_MACRO_API service_discovery_module();

 private:
  void update_keepalive_topology_value();
  void update_keepalive_discovery_value();
  int init_keepalives(service_discovery_cluster_context &context);
  int init_watchers(service_discovery_cluster_context &context);

 public:
  LIBATAPP_MACRO_API int init() override;
  LIBATAPP_MACRO_API const char *ATFW_UTIL_MACRO_NONNULL name() const override;
  LIBATAPP_MACRO_API int stop() override;

 public:
  // 使用相同的discovery数据 可以使用不同的etcd配置
  LIBATAPP_MACRO_API int add_service_discovery_cluster_context(etcd_cluster_load_conf_func_t load_conf_func);

  LIBATAPP_MACRO_API void set_maybe_update_keepalive_topology_value();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_discovery_value();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_discovery_area();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_discovery_metadata();

  LIBATAPP_MACRO_API std::string get_discovery_by_id_path(const std::string &path) const;
  LIBATAPP_MACRO_API std::string get_discovery_by_name_path(const std::string &path) const;
  LIBATAPP_MACRO_API std::string get_topology_path(const std::string &path) const;

  LIBATAPP_MACRO_API std::string get_discovery_by_id_watcher_path(const std::string &path) const;
  LIBATAPP_MACRO_API std::string get_discovery_by_name_watcher_path(const std::string &path) const;
  LIBATAPP_MACRO_API std::string get_topology_watcher_path(const std::string &path) const;

  LIBATAPP_MACRO_API int init_discovery_watcher_by_id(service_discovery_cluster_context &context);
  LIBATAPP_MACRO_API int init_discovery_watcher_by_name(service_discovery_cluster_context &context);
  LIBATAPP_MACRO_API int init_topology_watcher(service_discovery_cluster_context &context);
  LIBATAPP_MACRO_API int init_discovery_keepalive_by_id(service_discovery_cluster_context &context, std::string &value);
  LIBATAPP_MACRO_API int init_discovery_keepalive_by_name(service_discovery_cluster_context &context,
                                                          std::string &value);
  LIBATAPP_MACRO_API int init_topology_keepalive(service_discovery_cluster_context &context, std::string &value);

  LIBATAPP_MACRO_API void add_discovery_watcher_by_id_callback(const discovery_watcher_list_callback_t &fn);
  LIBATAPP_MACRO_API void add_discovery_watcher_by_name_callback(const discovery_watcher_list_callback_t &fn);
  LIBATAPP_MACRO_API void add_topology_watcher_callback(const topology_watcher_list_callback_t &fn);

  LIBATAPP_MACRO_API const ::atframework::atapp::etcd_cluster &get_raw_etcd_ctx() const;
  LIBATAPP_MACRO_API ::atframework::atapp::etcd_cluster &get_raw_etcd_ctx();

  LIBATAPP_MACRO_API const ::atframework::atapp::etcd_response_header &get_last_etcd_event_topology_header()
      const noexcept;
  LIBATAPP_MACRO_API const ::atframework::atapp::etcd_response_header &get_last_etcd_event_discovery_header()
      const noexcept;

  LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &get_configure() const;
  LIBATAPP_MACRO_API const std::string &get_configure_path() const;

  LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t add_keepalive_actor(etcd_cluster_holder &cluster_holder,
                                                                      std::string &val, const std::string &node_path);
  LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t add_keepalive_actor_default(std::string &val,
                                                                              const std::string &node_path);

  LIBATAPP_MACRO_API bool remove_keepalive_actor(etcd_cluster_holder &cluster_holder,
                                                 const atapp::etcd_keepalive::ptr_t &keepalive);
  LIBATAPP_MACRO_API bool remove_keepalive_actor_default(const atapp::etcd_keepalive::ptr_t &keepalive);

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
      service_discovery_cluster_context &context);
  LIBATAPP_MACRO_API etcd_watcher::watch_event_fn_t_ptr create_topology_watcher_callback_list_wrapper(
      service_discovery_cluster_context &context);

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
  service_discovery_cluster_context cluster_context_;
  std::list<std::shared_ptr<service_discovery_cluster_context>> cluster_context_list_;

  bool maybe_update_internal_keepalive_topology_value_;
  bool maybe_update_internal_keepalive_discovery_value_;
  bool maybe_update_internal_keepalive_discovery_area_;
  bool maybe_update_internal_keepalive_discovery_metadata_;

  atapp_topology_info_ptr_t last_submmited_topology_data_;
  atapp::protocol::atapp_area last_submmited_discovery_data_area_;
  atapp::protocol::atapp_metadata last_submmited_discovery_data_metadata_;

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
