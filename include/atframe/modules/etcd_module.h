// Copyright 2026 atframework
//
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <atframe/atapp_conf.h>

#include <network/http_request.h>
#include <time/time_utility.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_discovery.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <list>
#include <string>

LIBATAPP_MACRO_NAMESPACE_BEGIN

class app;

class etcd_module {
 public:
  LIBATAPP_MACRO_API etcd_module();
  LIBATAPP_MACRO_API virtual ~etcd_module();

 public:
  LIBATAPP_MACRO_API int init(atframework::atapp::app &app, const atapp::protocol::atapp_etcd &conf,
                              const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf);
  LIBATAPP_MACRO_API int reload(const atapp::protocol::atapp_etcd &conf,
                                const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf);
  LIBATAPP_MACRO_API int stop();
  LIBATAPP_MACRO_API void reset();
  LIBATAPP_MACRO_API int tick();

  LIBATAPP_MACRO_API bool is_cluster_closing() const { return cluster_.check_flag(etcd_cluster::flag_t::kClosing); }
 public:
  LIBATAPP_MACRO_API atapp::etcd_cluster &get_etcd_cluster();
  LIBATAPP_MACRO_API const atapp::etcd_cluster &get_etcd_cluster() const;
  LIBATAPP_MACRO_API const std::string &get_configure_path();
  LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &get_configure() const;

  LIBATAPP_MACRO_API bool check_keepalive_actor_start_success(
      gsl::span<const std::list<etcd_keepalive::ptr_t> *> keepalive_actors);

  LIBATAPP_MACRO_API static std::string generate_etcd_path(const std::string &path);

  LIBATAPP_MACRO_API bool is_etcd_enabled() const;

 private:
  static int http_callback_on_etcd_closed(atfw::util::network::http_request &req);
  void load_cluster_conf(const atapp::protocol::atapp_etcd &conf,
                         const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf = nullptr);

  bool enable_;
  bool load_conf_;
  atfw::util::network::http_request::ptr_t cleanup_request_;

  atframework::atapp::app *ATFW_UTIL_MACRO_NULLABLE atapp_;
  atapp::etcd_cluster cluster_;

  // Config
  std::string conf_path_cache_;
  atapp::protocol::atapp_etcd conf_cache_;
};
LIBATAPP_MACRO_NAMESPACE_END
