// Copyright 2026 atframework
//
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

#include <list>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

LIBATAPP_MACRO_NAMESPACE_BEGIN

// atapp_log nullptr 使用默认Logger
// atapp_log 非空但 category 为空不使用日志
class etcd_cluster_holder;
using etcd_cluster_load_conf_func_t = std::function<void(atframework::atapp::app &, etcd_cluster_holder &)>;
using etcd_cluster_init_keepalive_watcher_func_t =
    std::function<int(atframework::atapp::app &, etcd_cluster_holder &, bool init)>;
using etcd_cluster_reset_keepalive_watcher_func_t =
    std::function<void(atframework::atapp::app &, etcd_cluster_holder &)>;

using etcd_cluster_holder_ptr_t = std::shared_ptr<etcd_cluster_holder>;

class etcd_module;
class etcd_cluster_holder {
 public:
  friend class etcd_module;
  LIBATAPP_MACRO_API etcd_cluster_holder();
  LIBATAPP_MACRO_API atapp::etcd_cluster &get_etcd_cluster();
  LIBATAPP_MACRO_API const atapp::etcd_cluster &get_etcd_cluster() const;
  LIBATAPP_MACRO_API const std::string &get_configure_path() const;
  LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &get_configure() const;

 private:
  int tick();
  void stop();
  bool is_stopped() const;
  void reset();
  static int http_callback_on_etcd_closed(atfw::util::network::http_request &req);

  bool enable_;
  atfw::util::network::http_request::ptr_t cleanup_request_;

  atframework::atapp::app *ATFW_UTIL_MACRO_NULLABLE atapp_;
  etcd_module *ATFW_UTIL_MACRO_NULLABLE module_;
  etcd_cluster_load_conf_func_t load_conf_func_;
  etcd_cluster_init_keepalive_watcher_func_t init_keepalive_watcher_func_;
  etcd_cluster_reset_keepalive_watcher_func_t reset_keepalive_watcher_func_;
  atapp::etcd_cluster cluster_;

  // Config
  std::string conf_path_cache_;
  atapp::protocol::atapp_etcd conf_cache_;
  atfw::util::time::time_utility::raw_time_t tick_next_timepoint_;
  std::chrono::system_clock::duration tick_interval_;
};

class etcd_module : public ::atframework::atapp::module_impl {
 public:
  friend class etcd_cluster_holder;

  LIBATAPP_MACRO_API etcd_module();
  LIBATAPP_MACRO_API virtual ~etcd_module();

 public:
  LIBATAPP_MACRO_API void reset();

  LIBATAPP_MACRO_API int init() override;

 public:
  LIBATAPP_MACRO_API int reload() override;

  LIBATAPP_MACRO_API int stop() override;

  LIBATAPP_MACRO_API int timeout() override;

  LIBATAPP_MACRO_API const char *ATFW_UTIL_MACRO_NONNULL name() const override;

  LIBATAPP_MACRO_API int tick() override;

 public:
  LIBATAPP_MACRO_API int init_cluster(etcd_cluster_holder_ptr_t cluster_holder,
                                      etcd_cluster_load_conf_func_t load_conf_func,
                                      etcd_cluster_init_keepalive_watcher_func_t init_keepalive_watcher_func,
                                      etcd_cluster_reset_keepalive_watcher_func_t reset_keepalive_watcher_func);
  LIBATAPP_MACRO_API const std::string &get_conf_custom_data() const;
  LIBATAPP_MACRO_API void set_conf_custom_data(const std::string &v);

  LIBATAPP_MACRO_API bool is_etcd_enabled() const;
  LIBATAPP_MACRO_API void enable_etcd();
  LIBATAPP_MACRO_API void disable_etcd();

  LIBATAPP_MACRO_API const atfw::util::network::http_request::curl_m_bind_ptr_t &get_shared_curl_multi_context() const;
  LIBATAPP_MACRO_API static bool check_keepalive_actor_start_success(
      atframework::atapp::app &app, etcd_cluster_holder &cluster_holder,
      gsl::span<const std::list<etcd_keepalive::ptr_t> *> keepalive_actors);

  LIBATAPP_MACRO_API static std::string generate_etcd_path(const std::string &path);

  LIBATAPP_MACRO_API void load_cluster_conf(
      etcd_cluster_holder &cluster_holder, const atapp::protocol::atapp_etcd &conf,
      const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf = nullptr);

 private:
  bool etcd_enabled_;
  std::string custom_data_;
  atfw::util::network::http_request::curl_m_bind_ptr_t curl_multi_;
  atfw::util::log::log_wrapper::ptr_t logger_;
  std::list<etcd_cluster_holder_ptr_t> clusters_;
};
LIBATAPP_MACRO_NAMESPACE_END
