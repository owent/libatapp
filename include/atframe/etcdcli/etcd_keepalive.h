/**
 * etcd_keepalive.h
 *
 *  Created on: 2017-12-26
 *      Author: owent
 *
 *  Released under the MIT license
 */

#ifndef LIBATAPP_ETCDCLI_ETCD_KEEPALIVE_H
#define LIBATAPP_ETCDCLI_ETCD_KEEPALIVE_H

#pragma once

#include <string>

#include <std/functional.h>
#include <std/smart_ptr.h>

#include <config/compiler_features.h>

#include <network/http_request.h>

#include "etcd_def.h"

namespace atapp {
class etcd_cluster;

class etcd_keepalive : public std::enable_shared_from_this<etcd_keepalive> {
 public:
  typedef std::function<bool(const std::string &)> checker_fn_t;  // the parameter will be base64 of the value
  typedef std::shared_ptr<etcd_keepalive> ptr_t;

 private:
  struct default_checker_t {
    default_checker_t(const std::string &checked);
    ~default_checker_t();

    bool operator()(const std::string &checked) const;

    std::string data;
  };

  struct constrict_helper_t {};

 public:
  LIBATAPP_MACRO_API etcd_keepalive(etcd_cluster &owner, const std::string &path, constrict_helper_t &helper);
  LIBATAPP_MACRO_API ~etcd_keepalive();
  static LIBATAPP_MACRO_API ptr_t create(etcd_cluster &owner, const std::string &path);

  LIBATAPP_MACRO_API void close(bool reset_has_data_flag);

  LIBATAPP_MACRO_API void set_checker(const std::string &checked_str);
  LIBATAPP_MACRO_API void set_checker(checker_fn_t fn);

  LIBATAPP_MACRO_API void set_value(const std::string &str);
  LIBATAPP_MACRO_API void reset_value_changed();

  LIBATAPP_MACRO_API const std::string &get_value() const;

  LIBATAPP_MACRO_API const std::string &get_path() const;

  LIBATAPP_MACRO_API void active();

  UTIL_FORCEINLINE etcd_cluster &get_owner() { return *owner_; }
  UTIL_FORCEINLINE const etcd_cluster &get_owner() const { return *owner_; }

  UTIL_FORCEINLINE bool is_check_run() const { return checker_.is_check_run; }
  UTIL_FORCEINLINE bool is_check_passed() const { return checker_.is_check_passed; }
  UTIL_FORCEINLINE size_t get_check_times() const { return checker_.retry_times; }
  UTIL_FORCEINLINE bool has_data() const { return rpc_.has_data; }

 private:
  void process();

 private:
  static int libcurl_callback_on_get_data(util::network::http_request &req);
  static int libcurl_callback_on_set_data(util::network::http_request &req);

 private:
  etcd_cluster *owner_;
  std::string path_;
  std::string value_;
  struct rpc_data_t {
    util::network::http_request::ptr_t rpc_opr_;
    bool is_actived;
    bool is_value_changed;
    bool has_data;
  };
  rpc_data_t rpc_;

  struct checker_t {
    checker_fn_t fn;
    bool is_check_run;
    bool is_check_passed;
    size_t retry_times;
  };
  checker_t checker_;
};
}  // namespace atapp

#endif