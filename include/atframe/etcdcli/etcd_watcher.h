// Copyright 2021 atframework
// Created by owent on 2017-12-26

#pragma once

#include <config/compiler_features.h>

#include <gsl/select-gsl.h>
#include <network/http_request.h>

#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "atframe/etcdcli/etcd_def.h"

LIBATAPP_MACRO_NAMESPACE_BEGIN

class etcd_cluster;

class etcd_watcher {
 public:
  struct LIBATAPP_MACRO_API_HEAD_ONLY event_t {
    etcd_watch_event::type evt_type;
    etcd_key_value kv;
    etcd_key_value prev_kv;
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY response_t {
    int64_t watch_id;
    bool created;
    bool canceled;
    bool snapshot;
    int64_t compact_revision;
    std::vector<event_t> events;
  };

  using ptr_t = std::shared_ptr<etcd_watcher>;
  using watch_event_fn_t = std::function<void(const etcd_response_header &header, const response_t &evt_data)>;

 private:
  struct constrict_helper_t {};

 public:
  LIBATAPP_MACRO_API etcd_watcher(etcd_cluster &owner, const std::string &path, const std::string &range_end,
                                  constrict_helper_t &helper);
  LIBATAPP_MACRO_API ~etcd_watcher();
  static LIBATAPP_MACRO_API ptr_t create(etcd_cluster &owner, const std::string &path,
                                         const std::string &range_end = "+1");

  LIBATAPP_MACRO_API void close();

  LIBATAPP_MACRO_API const std::string &get_path() const;

  LIBATAPP_MACRO_API void active();

  ATFW_UTIL_FORCEINLINE etcd_cluster &get_owner() noexcept { return *owner_; }
  ATFW_UTIL_FORCEINLINE const etcd_cluster &get_owner() const noexcept { return *owner_; }

  // ====================== apis for configure ==================

  ATFW_UTIL_FORCEINLINE bool is_progress_notify_enabled() const noexcept { return rpc_.enable_progress_notify; }
  ATFW_UTIL_FORCEINLINE void set_progress_notify_enabled(bool v) noexcept { rpc_.enable_progress_notify = v; }

  ATFW_UTIL_FORCEINLINE bool is_prev_kv_enabled() const noexcept { return rpc_.enable_prev_kv; }
  ATFW_UTIL_FORCEINLINE void set_prev_kv_enabled(bool v) noexcept { rpc_.enable_prev_kv = v; }

  ATFW_UTIL_FORCEINLINE void set_conf_retry_interval(std::chrono::system_clock::duration v) noexcept {
    rpc_.retry_interval = v;
  }
  ATFW_UTIL_FORCEINLINE void set_conf_retry_interval_sec(time_t v) noexcept {
    set_conf_retry_interval(std::chrono::seconds(v));
  }
  ATFW_UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_retry_interval() const noexcept {
    return rpc_.retry_interval;
  }

  ATFW_UTIL_FORCEINLINE void set_conf_request_timeout(std::chrono::system_clock::duration v) noexcept {
    rpc_.request_timeout = v;
  }
  ATFW_UTIL_FORCEINLINE void set_conf_request_timeout_sec(time_t v) noexcept {
    set_conf_request_timeout(std::chrono::seconds(v));
  }
  ATFW_UTIL_FORCEINLINE void set_conf_request_timeout_min(time_t v) noexcept {
    set_conf_request_timeout(std::chrono::minutes(v));
  }
  ATFW_UTIL_FORCEINLINE void set_conf_request_timeout_hour(time_t v) noexcept {
    set_conf_request_timeout(std::chrono::hours(v));
  }
  ATFW_UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_request_timeout() const noexcept {
    return rpc_.request_timeout;
  }

  ATFW_UTIL_FORCEINLINE void set_conf_get_request_timeout(std::chrono::system_clock::duration v) noexcept {
    rpc_.get_request_timeout = v;
  }
  ATFW_UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_get_request_timeout() const noexcept {
    return rpc_.get_request_timeout;
  }

  ATFW_UTIL_FORCEINLINE void set_conf_startup_random_delay_min(std::chrono::system_clock::duration v) noexcept {
    rpc_.startup_random_delay_min = v;
  }
  ATFW_UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_startup_random_delay_min() const noexcept {
    return rpc_.startup_random_delay_min;
  }

  ATFW_UTIL_FORCEINLINE void set_conf_startup_random_delay_max(std::chrono::system_clock::duration v) noexcept {
    rpc_.startup_random_delay_max = v;
  }
  ATFW_UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_startup_random_delay_max() const noexcept {
    return rpc_.startup_random_delay_max;
  }

  // ====================== apis for events ==================
  ATFW_UTIL_FORCEINLINE void set_evt_handle(watch_event_fn_t &&fn) { evt_handle_ = std::move(fn); }

 private:
  void process();

 private:
  static int libcurl_callback_on_range_completed(atfw::util::network::http_request &req);

  static int libcurl_callback_on_watch_completed(atfw::util::network::http_request &req);
  static int libcurl_callback_on_watch_write(atfw::util::network::http_request &req, const char *inbuf, size_t inbufsz,
                                             const char *&outbuf, size_t &outbufsz);

 private:
  gsl::not_null<etcd_cluster *> owner_;
  std::string path_;
  std::string range_end_;
  std::stringstream rpc_data_stream_;
  int64_t rpc_data_brackets_;
  struct rpc_data_t {
    atfw::util::network::http_request::ptr_t rpc_opr_;
    bool is_actived;
    bool is_retry_mode;
    bool enable_progress_notify;
    bool enable_prev_kv;
    int64_t last_revision;
    std::chrono::system_clock::time_point watcher_next_request_time;
    std::chrono::system_clock::duration retry_interval;
    std::chrono::system_clock::duration request_timeout;
    std::chrono::system_clock::duration get_request_timeout;
    std::chrono::system_clock::duration startup_random_delay_min;
    std::chrono::system_clock::duration startup_random_delay_max;
  };
  rpc_data_t rpc_;

  watch_event_fn_t evt_handle_;
};
LIBATAPP_MACRO_NAMESPACE_END
