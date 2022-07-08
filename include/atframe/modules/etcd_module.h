// Copyright 2021 atframework
// Created by owent

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

#include <ctime>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace atapp {
class etcd_module : public ::atapp::module_impl {
 public:
  using node_action_t = etcd_discovery_action_t;

  struct LIBATAPP_MACRO_API_HEAD_ONLY node_info_t {
    atapp::protocol::atapp_discovery node_discovery;
    node_action_t::type action;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY node_list_t {
    std::list<node_info_t> nodes;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY watcher_sender_list_t {
    std::reference_wrapper<etcd_module> atapp_module;
    std::reference_wrapper<const ::atapp::etcd_response_header> etcd_header;
    std::reference_wrapper<const ::atapp::etcd_watcher::response_t> etcd_body;
    std::reference_wrapper<const ::atapp::etcd_watcher::event_t> event;
    std::reference_wrapper<const node_info_t> node;

    inline watcher_sender_list_t(etcd_module &m, const ::atapp::etcd_response_header &h,
                                 const ::atapp::etcd_watcher::response_t &b, const ::atapp::etcd_watcher::event_t &e,
                                 const node_info_t &n)
        : atapp_module(std::ref(m)),
          etcd_header(std::cref(h)),
          etcd_body(std::cref(b)),
          event(std::cref(e)),
          node(std::cref(n)) {}
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY watcher_sender_one_t {
    std::reference_wrapper<etcd_module> atapp_module;
    std::reference_wrapper<const ::atapp::etcd_response_header> etcd_header;
    std::reference_wrapper<const ::atapp::etcd_watcher::response_t> etcd_body;
    std::reference_wrapper<const ::atapp::etcd_watcher::event_t> event;
    std::reference_wrapper<node_info_t> node;

    inline watcher_sender_one_t(etcd_module &m, const ::atapp::etcd_response_header &h,
                                const ::atapp::etcd_watcher::response_t &b, const ::atapp::etcd_watcher::event_t &e,
                                node_info_t &n)
        : atapp_module(std::ref(m)),
          etcd_header(std::cref(h)),
          etcd_body(std::cref(b)),
          event(std::cref(e)),
          node(std::ref(n)) {}
  };

  using watcher_list_callback_t = std::function<void(watcher_sender_list_t &)>;
  using watcher_one_callback_t = std::function<void(watcher_sender_one_t &)>;
  using node_event_callback_t = std::function<void(node_action_t::type, const etcd_discovery_node::ptr_t &)>;
  using node_event_callback_list_t = std::list<node_event_callback_t>;
  using node_event_callback_handle_t = node_event_callback_list_t::iterator;
  using atapp_discovery_ptr_t = std::shared_ptr<atapp::protocol::atapp_discovery>;

 public:
  LIBATAPP_MACRO_API etcd_module();
  LIBATAPP_MACRO_API virtual ~etcd_module();

 public:
  LIBATAPP_MACRO_API void reset();

  LIBATAPP_MACRO_API int init() override;

 private:
  void update_keepalive_value();
  int init_keepalives();
  int init_watchers();

 public:
  LIBATAPP_MACRO_API int reload() override;

  LIBATAPP_MACRO_API int stop() override;

  LIBATAPP_MACRO_API int timeout() override;

  LIBATAPP_MACRO_API const char *name() const override;

  LIBATAPP_MACRO_API int tick() override;

  LIBATAPP_MACRO_API const std::string &get_conf_custom_data() const;
  LIBATAPP_MACRO_API void set_conf_custom_data(const std::string &v);

  LIBATAPP_MACRO_API bool is_etcd_enabled() const;
  LIBATAPP_MACRO_API void enable_etcd();
  LIBATAPP_MACRO_API void disable_etcd();
  LIBATAPP_MACRO_API void set_maybe_update_keepalive_value();

  LIBATAPP_MACRO_API const util::network::http_request::curl_m_bind_ptr_t &get_shared_curl_multi_context() const;

  LIBATAPP_MACRO_API std::string get_by_id_path() const;
  LIBATAPP_MACRO_API std::string get_by_type_id_path() const;
  LIBATAPP_MACRO_API std::string get_by_type_name_path() const;
  LIBATAPP_MACRO_API std::string get_by_name_path() const;
  LIBATAPP_MACRO_API std::string get_by_tag_path(const std::string &tag_name) const;

  LIBATAPP_MACRO_API std::string get_by_id_watcher_path() const;
  LIBATAPP_MACRO_API std::string get_by_type_id_watcher_path(uint64_t type_id) const;
  LIBATAPP_MACRO_API std::string get_by_type_name_watcher_path(const std::string &type_name) const;
  LIBATAPP_MACRO_API std::string get_by_name_watcher_path() const;
  LIBATAPP_MACRO_API std::string get_by_tag_watcher_path(const std::string &tag_name) const;

  LIBATAPP_MACRO_API int add_watcher_by_id(watcher_list_callback_t fn);
  LIBATAPP_MACRO_API int add_watcher_by_type_id(uint64_t type_id, watcher_one_callback_t fn);
  LIBATAPP_MACRO_API int add_watcher_by_type_name(const std::string &type_name, watcher_one_callback_t fn);
  LIBATAPP_MACRO_API int add_watcher_by_name(watcher_list_callback_t fn);
  LIBATAPP_MACRO_API int add_watcher_by_tag(const std::string &tag_name, watcher_one_callback_t fn);

  LIBATAPP_MACRO_API atapp::etcd_watcher::ptr_t add_watcher_by_custom_path(const std::string &custom_path,
                                                                           watcher_one_callback_t fn);

  LIBATAPP_MACRO_API const ::atapp::etcd_cluster &get_raw_etcd_ctx() const;
  LIBATAPP_MACRO_API ::atapp::etcd_cluster &get_raw_etcd_ctx();

  LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &get_configure() const;
  LIBATAPP_MACRO_API const std::string &get_configure_path() const;

  LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t add_keepalive_actor(std::string &val, const std::string &node_path);

  LIBATAPP_MACRO_API bool remove_keepalive_actor(const atapp::etcd_keepalive::ptr_t &keepalive);

  LIBATAPP_MACRO_API node_event_callback_handle_t add_on_node_discovery_event(node_event_callback_t fn);
  LIBATAPP_MACRO_API void remove_on_node_event(node_event_callback_handle_t &handle);

  LIBATAPP_MACRO_API etcd_discovery_set &get_global_discovery();
  LIBATAPP_MACRO_API const etcd_discovery_set &get_global_discovery() const;

 private:
  static bool unpack(node_info_t &out, const std::string &path, const std::string &json, bool reset_data);
  static void pack(const node_info_t &out, std::string &json);

  static int http_callback_on_etcd_closed(util::network::http_request &req);

  struct watcher_callback_list_wrapper_t {
    etcd_module *mod;
    std::list<watcher_list_callback_t> *callbacks;
    int64_t snapshot_index;
    bool has_insert_snapshot_index;

    watcher_callback_list_wrapper_t(etcd_module &m, std::list<watcher_list_callback_t> &cbks, int64_t index);
    ~watcher_callback_list_wrapper_t();
    void operator()(const ::atapp::etcd_response_header &header, const ::atapp::etcd_watcher::response_t &evt_data);
  };

  struct watcher_callback_one_wrapper_t {
    etcd_module *mod;
    watcher_one_callback_t callback;
    int64_t snapshot_index;
    bool has_insert_snapshot_index;

    watcher_callback_one_wrapper_t(etcd_module &m, watcher_one_callback_t cbk, int64_t index);
    ~watcher_callback_one_wrapper_t();
    void operator()(const ::atapp::etcd_response_header &header, const ::atapp::etcd_watcher::response_t &evt_data);
  };

  bool update_inner_watcher_event(node_info_t &node);
  void reset_inner_watchers_and_keepalives();

  struct watcher_internal_access_t {
    static void cleanup_old_nodes(etcd_module &mod, etcd_discovery_set::node_by_name_type &old_names,
                                  etcd_discovery_set::node_by_id_type &old_ids);
  };

 private:
  std::string conf_path_cache_;
  std::string custom_data_;
  util::network::http_request::curl_m_bind_ptr_t curl_multi_;
  util::network::http_request::ptr_t cleanup_request_;
  bool etcd_ctx_enabled_;
  bool maybe_update_inner_keepalive_value_;
  util::time::time_utility::raw_time_t tick_next_timepoint_;
  std::chrono::system_clock::duration tick_interval_;
  atapp::etcd_cluster etcd_ctx_;

  std::list<etcd_keepalive::ptr_t> inner_keepalive_actors_;
  std::string inner_keepalive_value_;

  std::set<int64_t> watcher_snapshot_index_;
  int64_t watcher_snapshot_index_allocator_;
  std::list<watcher_list_callback_t> watcher_by_id_callbacks_;
  std::list<watcher_list_callback_t> watcher_by_name_callbacks_;

  etcd_watcher::ptr_t inner_watcher_by_name_;
  etcd_watcher::ptr_t inner_watcher_by_id_;
  etcd_discovery_set global_discovery_;
  node_event_callback_list_t node_event_callbacks_;
};
}  // namespace atapp
