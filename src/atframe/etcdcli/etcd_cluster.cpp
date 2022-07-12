// Copyright 2021 atframework
// Created by owent

#include <assert.h>

#include <libatbus.h>

#include <std/explicit_declare.h>

#include <common/string_oprs.h>

#include <log/log_wrapper.h>

#include <config/compiler/template_prefix.h>

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <config/compiler/template_suffix.h>

#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <atframe/etcdcli/etcd_cluster.h>

// Patch for MSVC
#if defined(GetObject)
#  undef GetObject
#endif

namespace atapp {
/**
 * @note APIs just like this
 * @see https://coreos.com/etcd/docs/latest/dev-guide/api_reference_v3.html
 * @see https://coreos.com/etcd/docs/latest/dev-guide/apispec/swagger/rpc.swagger.json
 * @note KeyValue: { "key": "KEY", "create_revision": "number", "mod_revision": "number", "version": "number", "value":
 * "", "lease": "number" } Get data => curl http://localhost:2379/v3/kv/range -X POST -d '{"key": "KEY", "range_end":
 * ""}' # Response {"kvs":
 * [{...}], "more": "bool", "count": "COUNT"} Set data => curl http://localhost:2379/v3/kv/put -X POST -d '{"key":
 * "KEY", "value": "", "lease": "number", "prev_kv": "bool"}' Renew data => curl http://localhost:2379/v3/kv/put -X POST
 * -d '{"key": "KEY", "value": "", "prev_kv": "bool", "ignore_lease": true}' # Response {"header":{...}, "prev_kv":
 * {...}} Delete data => curl http://localhost:2379/v3/kv/deleterange -X POST -d '{"key": "KEY", "range_end": "",
 * "prev_kv": "bool"}' # Response {"header":{...}, "deleted": "number", "prev_kvs": [{...}]}
 *
 *   Watch => curl http://localhost:2379/v3/watch -XPOST -d '{"create_request":  {"key": "WATCH KEY", "range_end": "",
 * "prev_kv": true}
 * }' # Response {"header":{...},"watch_id":"ID","created":"bool", "canceled": "bool", "compact_revision": "REVISION",
 * "events":
 * [{"type": "PUT=0|DELETE=1", "kv": {...}, prev_kv": {...}"}]}
 *
 *   Allocate Lease => curl http://localhost:2379/v3/lease/grant -XPOST -d '{"TTL": 5, "ID": 0}'
 *       # Response {"header":{...},"ID":"ID","TTL":"5"}
 *   Keepalive Lease => curl http://localhost:2379/v3/lease/keepalive -XPOST -d '{"ID": 0}'
 *       # Response {"header":{...},"ID":"ID","TTL":"5"}
 *   Revoke Lease => curl http://localhost:2379/v3/kv/lease/revoke -XPOST -d '{"ID": 0}'
 *       # Response {"header":{...}}
 *
 *   List members => curl http://localhost:2379/v3/cluster/member/list -XPOST -d '{}'
 *       # Response {"header":{...},"members":[{"ID":"ID","name":"NAME","peerURLs":["peer url"],"clientURLs":["client
 * url"]}]}
 *
 *   Authorization Header => curl -H "Authorization: TOKEN"
 *   Authorization => curl http://localhost:2379/v3/auth/authenticate -XPOST -d '{"name": "username", "password":
 * "pass"}' # Response {"header":{...}, "token": "TOKEN"} # Return 401 if auth token invalid # Return 400 with {"error":
 * "etcdserver: user name is empty", "code": 3} if need TOKEN # Return 400 with {"error": "etcdsrm -rf erver:
 * authentication failed, ...", "code": 3} if username of password invalid Authorization Enable: curl -L
 * http://127.0.0.1:2379/v3/auth/user/add -XPOST -d '{"name": "root", "password": "3d91123233ffd36825bf2aca17808bfe"}'
 *       curl -L http://127.0.0.1:2379/v3/auth/role/add -XPOST -d '{"name": "root"}'
 *       curl -L http://127.0.0.1:2379/v3/auth/user/grant -XPOST -d '{"user": "root", "role": "root"}'
 *       curl -L http://127.0.0.1:2379/v3/auth/enable -XPOST -d '{}'
 *       curl -L http://127.0.0.1:2379/v3/auth/user/get -XPOST -X POST -d '{"name": "root"}'
 */

#define ETCD_API_V3_ERROR_HTTP_CODE_AUTH 401
#define ETCD_API_V3_ERROR_HTTP_INVALID_PARAM 400
#define ETCD_API_V3_ERROR_HTTP_PRECONDITION 412
// @see https://godoc.org/google.golang.org/grpc/codes
#define ETCD_API_V3_ERROR_GRPC_CODE_UNAUTHENTICATED 16

#define ETCD_API_V3_MEMBER_LIST "/v3/cluster/member/list"
#define ETCD_API_V3_AUTH_AUTHENTICATE "/v3/auth/authenticate"
#define ETCD_API_V3_AUTH_USER_GET "/v3/auth/user/get"

#define ETCD_API_V3_KV_GET "/v3/kv/range"
#define ETCD_API_V3_KV_SET "/v3/kv/put"
#define ETCD_API_V3_KV_DELETE "/v3/kv/deleterange"

#define ETCD_API_V3_WATCH "/v3/watch"

#define ETCD_API_V3_LEASE_GRANT "/v3/lease/grant"
#define ETCD_API_V3_LEASE_KEEPALIVE "/v3/lease/keepalive"
#define ETCD_API_V3_LEASE_REVOKE "/v3/kv/lease/revoke"

namespace details {
static const std::string &get_default_user_agent() {
  static std::string ret;
  if (!ret.empty()) {
    return ret;
  }

  char buffer[256] = {0};
  const char *prefix = "Mozilla/5.0";
  const char *suffix = "Atapp Etcdcli/1.0";
#if defined(_WIN32) || defined(__WIN32__)
#  if (defined(__MINGW32__) && __MINGW32__)
  const char *sys_env = "Win32; MinGW32";
#  elif (defined(__MINGW64__) || __MINGW64__)
  const char *sys_env = "Win64; x64; MinGW64";
#  elif defined(__CYGWIN__) || defined(__MSYS__)
#    if defined(_WIN64) || defined(__amd64) || defined(__x86_64)
  const char *sys_env = "Win64; x64; POSIX";
#    else
  const char *sys_env = "Win32; POSIX";
#    endif
#  elif defined(_WIN64) || defined(__amd64) || defined(__x86_64)
  const char *sys_env = "Win64; x64";
#  else
  const char *sys_env = "Win32";
#  endif
#elif defined(__linux__) || defined(__linux)
  const char *sys_env = "Linux";
#elif defined(__APPLE__)
  const char *sys_env = "Darwin";
#elif defined(__DragonFly__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__OpenBSD__) || \
    defined(__NetBSD__)
  const char *sys_env = "BSD";
#elif defined(__unix__) || defined(__unix)
  const char *sys_env = "Unix";
#else
  const char *sys_env = "Unknown";
#endif

  UTIL_STRFUNC_SNPRINTF(buffer, sizeof(buffer) - 1, "%s (%s) %s", prefix, sys_env, suffix);
  ret = &buffer[0];

  return ret;
}

EXPLICIT_UNUSED_ATTR static int etcd_cluster_verbose_callback(util::network::http_request &req, curl_infotype type,
                                                              char *data, size_t size,
                                                              const util::log::log_wrapper::ptr_t &logger) {
  if (util::log::log_wrapper::check_level(WDTLOGGETCAT(util::log::log_wrapper::categorize_t::DEFAULT),
                                          util::log::log_wrapper::level_t::LOG_LW_TRACE) ||
      (logger && logger->check_level(util::log::log_wrapper::level_t::LOG_LW_TRACE))) {
    const char *verbose_type = "Unknown Action";
    switch (type) {
      case CURLINFO_TEXT:
        verbose_type = "Text";
        break;
      case CURLINFO_HEADER_OUT:
        verbose_type = "Header Send";
        break;
      case CURLINFO_DATA_OUT:
        verbose_type = "Data Send";
        break;
      case CURLINFO_SSL_DATA_OUT:
        verbose_type = "SSL Data Send";
        break;
      case CURLINFO_HEADER_IN:
        verbose_type = "Header Received";
        break;
      case CURLINFO_DATA_IN:
        verbose_type = "Data Received";
        break;
      case CURLINFO_SSL_DATA_IN:
        verbose_type = "SSL Data Received";
        break;
      default: /* in case a new one is introduced to shock us */
        break;
    }

    util::log::log_wrapper::caller_info_t caller = WDTLOGFILENF(util::log::log_wrapper::level_t::LOG_LW_TRACE, "Trace");
    WDTLOGGETCAT(util::log::log_wrapper::categorize_t::DEFAULT)
        ->log(caller, "Etcd cluster %p http request %p to %s => Verbose: %s", req.get_priv_data(), &req,
              req.get_url().c_str(), verbose_type);
    WDTLOGGETCAT(util::log::log_wrapper::categorize_t::DEFAULT)->write_log(caller, data, size);
    if (logger && logger->check_level(util::log::log_wrapper::level_t::LOG_LW_TRACE)) {
      logger->log(caller, "Etcd cluster %p http request %p to %s => Verbose: %s", req.get_priv_data(), &req,
                  req.get_url().c_str(), verbose_type);
      logger->write_log(caller, data, size);
    }
  }

  return 0;
}
}  // namespace details

LIBATAPP_MACRO_API etcd_cluster::etcd_cluster()
    : flags_(0),
      startup_log_level_(util::log::log_formatter::level_t::LOG_LW_DISABLED),
      runtime_log_level_(util::log::log_formatter::level_t::LOG_LW_DISABLED) {
  conf_.authorization_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.authorization_retry_interval = std::chrono::seconds(5);
  conf_.auth_user_get_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.auth_user_get_retry_interval = std::chrono::minutes(2);
  conf_.http_request_timeout = std::chrono::seconds(10);
  conf_.http_initialization_timeout = std::chrono::seconds(3);
  conf_.http_connect_timeout = std::chrono::seconds(5);
  conf_.dns_cache_timeout = std::chrono::seconds(0);
  conf_.dns_servers.clear();
  conf_.etcd_members_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.etcd_members_update_interval = std::chrono::minutes(5);
  conf_.etcd_members_retry_interval = std::chrono::minutes(1);
  conf_.etcd_members_init_retry_interval = std::chrono::seconds(3);

  conf_.lease = 0;
  conf_.keepalive_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.keepalive_timeout = std::chrono::seconds(16);
  conf_.keepalive_interval = std::chrono::seconds(5);
  conf_.keepalive_retry_interval = std::chrono::seconds(3);
  conf_.keepalive_retry_times = 8;

  conf_.ssl_enable_alpn = true;
  conf_.ssl_verify_peer = false;
  conf_.http_debug_mode = false;
  conf_.auto_update_hosts = true;

  conf_.ssl_min_version = ssl_version_t::DISABLED;
  conf_.user_agent.clear();
  conf_.proxy.clear();
  conf_.no_proxy.clear();
  conf_.proxy_user_name.clear();
  conf_.proxy_password.clear();

  conf_.ssl_client_cert.clear();
  conf_.ssl_client_cert_type.clear();
  conf_.ssl_client_key.clear();
  conf_.ssl_client_key_type.clear();
  conf_.ssl_client_key_passwd.clear();
  conf_.ssl_ca_cert.clear();

  conf_.ssl_proxy_cert.clear();
  conf_.ssl_proxy_cert_type.clear();
  conf_.ssl_proxy_key.clear();
  conf_.ssl_proxy_key_type.clear();
  conf_.ssl_proxy_key_passwd.clear();
  conf_.ssl_proxy_ca_cert.clear();

  conf_.ssl_cipher_list.clear();
  conf_.ssl_cipher_list_tls13.clear();

  memset(&stats_, 0, sizeof(stats_));
}

LIBATAPP_MACRO_API etcd_cluster::~etcd_cluster() {
  reset();
  cleanup_keepalive_deletors();
}

LIBATAPP_MACRO_API void etcd_cluster::init(const util::network::http_request::curl_m_bind_ptr_t &curl_mgr) {
  curl_multi_ = curl_mgr;
  random_generator_.init_seed(static_cast<util::random::mt19937::result_type>(util::time::time_utility::get_now()));

  set_flag(flag_t::CLOSING, false);
}

LIBATAPP_MACRO_API util::network::http_request::ptr_t etcd_cluster::close(bool wait, bool revoke_lease) {
  set_flag(flag_t::CLOSING, true);
  set_flag(flag_t::RUNNING, false);

  if (rpc_keepalive_) {
    rpc_keepalive_->set_on_complete(nullptr);
    rpc_keepalive_->stop();
    rpc_keepalive_.reset();
  }

  cleanup_keepalive_deletors();

  if (rpc_update_members_) {
    rpc_update_members_->set_on_complete(nullptr);
    rpc_update_members_->stop();
    rpc_update_members_.reset();
  }

  if (rpc_authenticate_) {
    rpc_authenticate_->set_on_complete(nullptr);
    rpc_authenticate_->stop();
    rpc_authenticate_.reset();
  }

  for (size_t i = 0; i < keepalive_actors_.size(); ++i) {
    if (keepalive_actors_[i]) {
      keepalive_actors_[i]->close(false);
    }
  }
  keepalive_actors_.clear();
  keepalive_retry_actors_.clear();

  for (size_t i = 0; i < watcher_actors_.size(); ++i) {
    if (watcher_actors_[i]) {
      watcher_actors_[i]->close();
    }
  }
  watcher_actors_.clear();

  util::network::http_request::ptr_t ret;
  if (curl_multi_) {
    if (0 != conf_.lease) {
      if (revoke_lease) {
        ret = create_request_lease_revoke();

        // wait to delete content
        if (ret) {
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*this, "Etcd start to revoke lease {}", get_lease());
          ret->start(util::network::http_request::method_t::EN_MT_POST, wait);
        }
      }

      conf_.lease = 0;
    }
  }

  if (ret && false == ret->is_running()) {
    ret.reset();
  }

  return ret;
}

LIBATAPP_MACRO_API void etcd_cluster::reset() {
  close(true, true);

  curl_multi_.reset();
  flags_ = 0;

  conf_.http_request_timeout = std::chrono::seconds(10);
  conf_.http_initialization_timeout = std::chrono::seconds(3);
  conf_.http_connect_timeout = std::chrono::seconds(5);
  conf_.dns_cache_timeout = std::chrono::seconds(0);
  conf_.dns_servers.clear();

  conf_.authorization_user_roles.clear();
  conf_.authorization_header.clear();
  conf_.authorization_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.authorization_retry_interval = std::chrono::seconds(5);
  conf_.auth_user_get_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.auth_user_get_retry_interval = std::chrono::minutes(2);
  conf_.path_node.clear();
  conf_.etcd_members_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.etcd_members_update_interval = std::chrono::minutes(5);
  conf_.etcd_members_retry_interval = std::chrono::minutes(1);
  conf_.etcd_members_init_retry_interval = std::chrono::seconds(3);

  conf_.lease = 0;
  conf_.keepalive_next_update_time = std::chrono::system_clock::from_time_t(0);
  conf_.keepalive_timeout = std::chrono::seconds(16);
  conf_.keepalive_interval = std::chrono::seconds(5);
  conf_.keepalive_retry_interval = std::chrono::seconds(3);
  conf_.keepalive_retry_times = 8;

  conf_.ssl_enable_alpn = true;
  conf_.ssl_verify_peer = false;
  conf_.http_debug_mode = false;
  conf_.auto_update_hosts = true;

  conf_.ssl_min_version = ssl_version_t::DISABLED;
  conf_.user_agent.clear();
  conf_.proxy.clear();
  conf_.no_proxy.clear();
  conf_.proxy_user_name.clear();
  conf_.proxy_password.clear();

  conf_.ssl_client_cert.clear();
  conf_.ssl_client_cert_type.clear();
  conf_.ssl_client_key.clear();
  conf_.ssl_client_key_type.clear();
  conf_.ssl_client_key_passwd.clear();
  conf_.ssl_ca_cert.clear();

  conf_.ssl_proxy_cert.clear();
  conf_.ssl_proxy_cert_type.clear();
  conf_.ssl_proxy_key.clear();
  conf_.ssl_proxy_key_type.clear();
  conf_.ssl_proxy_key_passwd.clear();
  conf_.ssl_proxy_ca_cert.clear();

  conf_.ssl_cipher_list.clear();
  conf_.ssl_cipher_list_tls13.clear();
}

LIBATAPP_MACRO_API int etcd_cluster::tick() {
  int ret = 0;

  if (!curl_multi_) {
    return ret;
  }

  if (check_flag(flag_t::CLOSING)) {
    return 0;
  }

  // update members
  if (util::time::time_utility::sys_now() > conf_.etcd_members_next_update_time) {
    ret += create_request_member_update() ? 1 : 0;
  }

  // empty other actions will be delayed
  if (conf_.path_node.empty()) {
    return ret;
  }

  // check or start authorization
  if (!check_authorization()) {
    if (!rpc_authenticate_) {
      ret += create_request_auth_authenticate() ? 1 : 0;
    }

    return ret;
  }

  // Send /v3/auth/user/get interval to renew auth token
  if (!conf_.authorization.empty() && !rpc_authenticate_) {
    ret += create_request_auth_user_get() ? 1 : 0;
  }

  // keepalive lease
  if (check_flag(flag_t::ENABLE_LEASE)) {
    if (0 == get_lease()) {
      ret += create_request_lease_grant() ? 1 : 0;

      // run actions after lease granted
      return ret;
    } else if (util::time::time_utility::sys_now() > conf_.keepalive_next_update_time) {
      ret += create_request_lease_keepalive() ? 1 : 0;
    }
  } else if (!check_flag(flag_t::RUNNING)) {
    set_flag(flag_t::RUNNING, true);
  }

  // run pending
  retry_pending_actions();

  // resolve ready flag
  if (!check_flag(flag_t::READY) && check_flag(flag_t::RUNNING)) {
    resolve_ready();
  }

  return ret;
}

LIBATAPP_MACRO_API bool etcd_cluster::is_available() const {
  if (!curl_multi_) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  // empty other actions will be delayed
  if (conf_.path_node.empty()) {
    return false;
  }

  // check or start authorization
  return check_authorization();
}

LIBATAPP_MACRO_API void etcd_cluster::resolve_ready() noexcept {
  if (check_flag(flag_t::READY) || !check_flag(flag_t::RUNNING)) {
    return;
  }

  bool ready = true;
  for (auto &keepalive : keepalive_actors_) {
    if (!keepalive) {
      continue;
    }
    if (!keepalive->is_check_run() || !keepalive->is_check_passed()) {
      ready = false;
      break;
    }
  }

  if (ready) {
    set_flag(flag_t::READY, true);
  }
}

LIBATAPP_MACRO_API void etcd_cluster::set_flag(flag_t::type f, bool v) {
  assert(0 == (f & (f - 1)));
  if (v == check_flag(f)) {
    return;
  }

  if (v) {
    flags_ |= f;
  } else {
    flags_ &= ~f;
  }

  switch (f) {
    case flag_t::ENABLE_LEASE: {
      if (v) {
        create_request_lease_grant();
      } else if (rpc_keepalive_) {
        rpc_keepalive_->set_on_complete(nullptr);
        rpc_keepalive_->stop();
        rpc_keepalive_.reset();
      }
      break;
    }
    case flag_t::RUNNING: {
      if (v) {
        for (on_event_up_down_handle_set_t::iterator iter = event_on_up_callbacks_.begin();
             iter != event_on_up_callbacks_.end(); ++iter) {
          if (*iter) {
            (*iter)(*this);
          }
        }
      } else {
        for (on_event_up_down_handle_set_t::iterator iter = event_on_down_callbacks_.begin();
             iter != event_on_down_callbacks_.end(); ++iter) {
          if (*iter) {
            (*iter)(*this);
          }
        }
      }
      break;
    }
    case flag_t::READY: {
      if (v && logger_) {
        logger_->set_level(runtime_log_level_);
      }
      break;
    }
    default: {
      break;
    }
  }
}

LIBATAPP_MACRO_API void etcd_cluster::set_logger(const util::log::log_wrapper::ptr_t &logger,
                                                 util::log::log_formatter::level_t::type log_level) noexcept {
  logger_ = logger;
  startup_log_level_ = log_level;
  if (logger) {
    runtime_log_level_ = logger->get_level();
  }

  if (logger_ && !check_flag(flag_t::READY)) {
    logger_->set_level(startup_log_level_);
  }
}

LIBATAPP_MACRO_API void etcd_cluster::pick_conf_authorization(std::string &username, std::string *password) {
  std::string::size_type username_sep = conf_.authorization.find(':');
  if (username_sep == std::string::npos) {
    username = conf_.authorization;
  } else {
    username = conf_.authorization.substr(0, username_sep);
    if (nullptr != password && username_sep + 1 < conf_.authorization.size()) {
      *password = conf_.authorization.substr(username_sep + 1);
    }
  }
}

LIBATAPP_MACRO_API time_t etcd_cluster::get_http_timeout_ms() const noexcept {
  time_t ret =
      static_cast<time_t>(std::chrono::duration_cast<std::chrono::milliseconds>(get_conf_http_timeout()).count());
  if (ret <= 0) {
    ret = 30000;  // 30s
  }

  return ret;
}

LIBATAPP_MACRO_API time_t etcd_cluster::get_http_connect_timeout_ms() const noexcept {
  time_t ret = static_cast<time_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(get_conf_http_connect_timeout()).count());

  return ret;
}

LIBATAPP_MACRO_API bool etcd_cluster::add_keepalive(const std::shared_ptr<etcd_keepalive> &keepalive) {
  if (!keepalive) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  if (keepalive_actors_.end() != std::find(keepalive_actors_.begin(), keepalive_actors_.end(), keepalive)) {
    return false;
  }

  if (this != &keepalive->get_owner()) {
    return false;
  }

  set_flag(flag_t::ENABLE_LEASE, true);
  keepalive_actors_.push_back(keepalive);

  etcd_keepalive_deletor_map_t::iterator iter = keepalive_deletors_.find(keepalive->get_path());
  if (iter != keepalive_deletors_.end()) {
    etcd_keepalive_deletor *keepalive_deletor = iter->second;
    keepalive_deletors_.erase(iter);
    delete_keepalive_deletor(keepalive_deletor, true);
  }

  // auto active if cluster is running
  if (check_flag(flag_t::RUNNING) && 0 != get_lease()) {
    keepalive->active();
  }
  return true;
}

LIBATAPP_MACRO_API bool etcd_cluster::add_retry_keepalive(const std::shared_ptr<etcd_keepalive> &keepalive) {
  if (!keepalive) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  if (keepalive_retry_actors_.end() !=
      std::find(keepalive_retry_actors_.begin(), keepalive_retry_actors_.end(), keepalive)) {
    return false;
  }

  if (this != &keepalive->get_owner()) {
    return false;
  }

  set_flag(flag_t::ENABLE_LEASE, true);
  keepalive_retry_actors_.push_back(keepalive);
  return true;
}

LIBATAPP_MACRO_API bool etcd_cluster::remove_keepalive(std::shared_ptr<etcd_keepalive> keepalive) {
  if (!keepalive) {
    return false;
  }

  bool found = false;
  for (size_t i = 0; i < keepalive_actors_.size(); ++i) {
    if (keepalive_actors_[i] == keepalive) {
      if (i != keepalive_actors_.size() - 1) {
        keepalive_actors_[i].swap(keepalive_actors_[keepalive_actors_.size() - 1]);
      }

      keepalive_actors_.pop_back();
      found = true;
      break;
    }
  }

  for (size_t i = 0; i < keepalive_retry_actors_.size(); ++i) {
    if (keepalive_retry_actors_[i] == keepalive) {
      if (i != keepalive_retry_actors_.size() - 1) {
        keepalive_retry_actors_[i].swap(keepalive_retry_actors_[keepalive_retry_actors_.size() - 1]);
      }

      keepalive_retry_actors_.pop_back();
      found = true;
      break;
    }
  }

  if (found) {
    if (keepalive->has_data()) {
      etcd_keepalive_deletor *keepalive_deletor = new etcd_keepalive_deletor();
      if (nullptr == keepalive_deletor) {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
            *this, "Etcd cluster try to delete keepalive {} path {} but malloc etcd_keepalive_deletor failed.",
            reinterpret_cast<const void *>(keepalive.get()), keepalive->get_path());
      } else {
        keepalive_deletor->retry_times = 0;
        keepalive_deletor->path = keepalive->get_path();
        keepalive_deletor->keepalive_addr = keepalive.get();
        keepalive_deletor->owner = nullptr;

        remove_keepalive_path(keepalive_deletor, false);
      }

      keepalive->close(true);
    } else {
      keepalive->close(false);
    }
  }

  return found;
}

LIBATAPP_MACRO_API bool etcd_cluster::add_watcher(const std::shared_ptr<etcd_watcher> &watcher) {
  if (!watcher) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  if (watcher_actors_.end() != std::find(watcher_actors_.begin(), watcher_actors_.end(), watcher)) {
    return false;
  }

  if (this != &watcher->get_owner()) {
    return false;
  }

  watcher_actors_.push_back(watcher);
  return true;
}

LIBATAPP_MACRO_API bool etcd_cluster::remove_watcher(std::shared_ptr<etcd_watcher> watcher) {
  if (!watcher) {
    return false;
  }

  bool has_data = false;
  for (size_t i = 0; i < watcher_actors_.size(); ++i) {
    if (watcher_actors_[i] == watcher) {
      if (i != watcher_actors_.size() - 1) {
        watcher_actors_[i].swap(watcher_actors_[watcher_actors_.size() - 1]);
      }

      watcher_actors_.pop_back();
      has_data = true;
      break;
    }
  }

  if (has_data) {
    watcher->close();
  }

  return has_data;
}

void etcd_cluster::remove_keepalive_path(etcd_keepalive_deletor *keepalive_deletor, bool delay_delete) {
  if (nullptr == keepalive_deletor) {
    return;
  }

  etcd_keepalive_deletor *delete_old_deletor = nullptr;

  {
    etcd_keepalive_deletor_map_t::iterator iter = keepalive_deletors_.find(keepalive_deletor->path);
    if (iter != keepalive_deletors_.end()) {
      delete_old_deletor = iter->second;

      if (delete_old_deletor == keepalive_deletor) {
        delete_old_deletor = nullptr;
      }

      keepalive_deletors_.erase(iter);
    }
  }

  do {
    if (delay_delete) {
      // insert and retry later
      keepalive_deletors_[keepalive_deletor->path] = keepalive_deletor;
      break;
    }

    keepalive_deletor->owner = this;

    ++keepalive_deletor->retry_times;
    // retry at most 5 times
    size_t max_retry_times = conf_.keepalive_retry_times;
    if (0 == max_retry_times) {
      max_retry_times = 8;
    }
    if (keepalive_deletor->retry_times > max_retry_times) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
          *this, "Etcd cluster try to delete keepalive {} path {} too many times, skip to retry again.",
          keepalive_deletor->keepalive_addr, keepalive_deletor->path);
      delete_keepalive_deletor(keepalive_deletor, true);
      break;
    }

    util::network::http_request::ptr_t rpc = create_request_kv_del(keepalive_deletor->path, "+1");
    if (!rpc) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this, "Etcd cluster create delete keepalive {} path request to {} failed",
                                            keepalive_deletor->keepalive_addr, keepalive_deletor->path);

      // insert and retry later
      keepalive_deletors_[keepalive_deletor->path] = keepalive_deletor;
      break;
    }

    keepalive_deletor->rpc = rpc;
    rpc->set_on_complete(etcd_cluster::libcurl_callback_on_remove_keepalive_path);
    rpc->set_priv_data(keepalive_deletor);

    int res = rpc->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this,
                                            "Etcd cluster start delete keepalive {} request to {} failed, res: {}",
                                            reinterpret_cast<const void *>(this), rpc->get_url(), res);
      rpc->set_on_complete(nullptr);
      rpc->set_priv_data(nullptr);
      keepalive_deletor->rpc.reset();

      // insert and retry later
      keepalive_deletors_[keepalive_deletor->path] = keepalive_deletor;
    } else {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*this, "Etcd cluster start delete keepalive {} request to {} success",
                                            reinterpret_cast<const void *>(this), rpc->get_url());
    }

  } while (false);

  if (nullptr != delete_old_deletor) {
    delete_keepalive_deletor(delete_old_deletor, true);
  }
}  // namespace atapp

int etcd_cluster::libcurl_callback_on_remove_keepalive_path(util::network::http_request &req) {
  etcd_keepalive_deletor *self = reinterpret_cast<etcd_keepalive_deletor *>(req.get_priv_data());
  if (nullptr == self) {
    // Maybe deleted before or in callback, just skip
    return 0;
  }

  assert(self->keepalive_addr);
  do {
    if (nullptr == self) {
      FWLOGERROR("Etcd cluster delete keepalive path shouldn't has request without private data");
      break;
    }

    if (self->rpc.get() == &req) {
      self->rpc.reset();
    }

    // 服务器错误则忽略，正常流程path不存在也会返回200，然后没有 deleted=1 。如果删除成功会有 deleted=1
    // 判定 404 只是是个防御性判定
    if (0 != req.get_error_code() ||
        (util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
             util::network::http_request::get_status_code_group(req.get_response_code()) &&
         util::network::http_request::status_code_t::EN_SCT_NOT_FOUND != req.get_response_code())) {
      if (nullptr != self->owner) {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
            *self->owner, "Etcd cluster delete keepalive {} path {} finished, res: {}, http code: {}\n{}",
            self->keepalive_addr, self->path, req.get_error_code(), req.get_response_code(), req.get_error_msg());
      } else {
        FWLOGERROR("Etcd cluster delete keepalive {} path {} failed, error code: {}, http code: {}\n{}",
                   self->keepalive_addr, self->path, req.get_error_code(), req.get_response_code(),
                   req.get_error_msg());
      }

      if (nullptr != self->owner) {
        // only network error will trigger a etcd member update
        if (0 != req.get_error_code()) {
          self->owner->retry_request_member_update(req.get_url());
        }
        self->owner->add_stats_error_request();

        self->owner->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());

        self->owner->remove_keepalive_path(self, true);
      } else {
        delete_keepalive_deletor(self, false);
      }

      return 0;
    }

    if (nullptr != self->owner) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(
          *self->owner, "Etcd cluster delete keepalive {} path {} finished, res: {}, http code: {}\n{}",
          self->keepalive_addr, self->path, req.get_error_code(), req.get_response_code(), req.get_error_msg());
    } else {
      FWLOGINFO("Etcd cluster delete keepalive {} path {} finished, res: {}, http code: {}\n{}", self->keepalive_addr,
                self->path, req.get_error_code(), req.get_response_code(), req.get_error_msg());
    }
  } while (false);

  delete_keepalive_deletor(self, false);
  return 0;
}

void etcd_cluster::retry_pending_actions() {
  // retry keepalive in retry list
  if (0 != get_lease()) {
    for (size_t i = 0; i < keepalive_retry_actors_.size(); ++i) {
      if (keepalive_retry_actors_[i]) {
        keepalive_retry_actors_[i]->reset_value_changed();
        keepalive_retry_actors_[i]->active();
      }
    }
    keepalive_retry_actors_.clear();
  }

  // reactive watcher
  for (size_t i = 0; i < watcher_actors_.size(); ++i) {
    if (watcher_actors_[i]) {
      watcher_actors_[i]->active();
    }
  }

  // retry keepalive deletors
  if (!keepalive_deletors_.empty()) {
    etcd_keepalive_deletor_map_t pending_deletes;
    pending_deletes.swap(keepalive_deletors_);

    for (etcd_keepalive_deletor_map_t::iterator iter = pending_deletes.begin(); iter != pending_deletes.end(); ++iter) {
      remove_keepalive_path(iter->second, false);
    }
  }
}

void etcd_cluster::set_lease(int64_t v, bool force_active_keepalives) {
  int64_t old_v = get_lease();
  conf_.lease = v;

  if (old_v == v && false == force_active_keepalives && v != 0) {
    retry_pending_actions();
    return;
  }

  if (0 != v) {
    keepalive_retry_actors_.clear();

    // all keepalive object start a update/set request
    for (size_t i = 0; i < keepalive_actors_.size(); ++i) {
      if (keepalive_actors_[i]) {
        keepalive_actors_[i]->reset_value_changed();
        keepalive_actors_[i]->active();
      }
    }

    retry_pending_actions();
  }
}

bool etcd_cluster::create_request_auth_authenticate() {
  if (!curl_multi_) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  if (conf_.path_node.empty()) {
    return false;
  }

  if (conf_.authorization.empty()) {
    conf_.authorization_header.clear();
    return false;
  }

  if (util::time::time_utility::sys_now() <= conf_.authorization_next_update_time) {
    return false;
  }

  if (rpc_authenticate_) {
    rpc_authenticate_->stop();
    rpc_authenticate_.reset();
  }

  if (std::chrono::system_clock::duration::zero() >= conf_.authorization_retry_interval) {
    conf_.authorization_next_update_time = util::time::time_utility::sys_now() + std::chrono::seconds(3);
  } else {
    conf_.authorization_next_update_time = util::time::time_utility::sys_now() + conf_.authorization_retry_interval;
  }

  util::network::http_request::ptr_t req = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_AUTH_AUTHENTICATE));

  std::string username, password;
  pick_conf_authorization(username, &password);

  if (req) {
    add_stats_create_request();

    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember("name", rapidjson::StringRef(username.c_str(), username.size()), doc.GetAllocator());
    doc.AddMember("password", rapidjson::StringRef(password.c_str(), password.size()), doc.GetAllocator());

    setup_http_request(req, doc, get_http_timeout_ms());
    req->set_priv_data(this);
    req->set_on_complete(libcurl_callback_on_auth_authenticate);

    if (get_conf_http_debug_mode()) {
      util::log::log_wrapper::ptr_t logger = logger_;
      req->set_on_verbose([logger](util::network::http_request &req, curl_infotype type, char *data, size_t size) {
        return details::etcd_cluster_verbose_callback(req, type, data, size, logger);
      });
    }
    int res = req->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      req->set_on_complete(nullptr);
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this, "Etcd start authenticate request for user {} to {} failed, res: {}",
                                            username, req->get_url(), res);
      add_stats_error_request();
      return false;
    }

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(*this, "Etcd start authenticate request for user {} to {}", username,
                                         req->get_url());
    rpc_authenticate_ = req;
    if (std::chrono::system_clock::duration::zero() >= conf_.auth_user_get_retry_interval) {
      conf_.auth_user_get_next_update_time = util::time::time_utility::sys_now() + std::chrono::seconds(3);
    } else {
      conf_.auth_user_get_next_update_time = util::time::time_utility::sys_now() + conf_.auth_user_get_retry_interval;
    }
  } else {
    add_stats_error_request();
  }

  return !!rpc_authenticate_;
}

int etcd_cluster::libcurl_callback_on_auth_authenticate(util::network::http_request &req) {
  etcd_cluster *self = reinterpret_cast<etcd_cluster *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd authenticate shouldn't has request without private data");
    return 0;
  }

  util::network::http_request::ptr_t keep_rpc = self->rpc_authenticate_;
  self->rpc_authenticate_.reset();

  // 服务器错误则忽略
  if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                       util::network::http_request::get_status_code_group(req.get_response_code())) {
    // only network error will trigger a etcd member update
    if (0 != req.get_error_code()) {
      self->retry_request_member_update(req.get_url());
    }
    self->add_stats_error_request();

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd authenticate failed, error code: {}, http code: {}\n{}",
                                          req.get_error_code(), req.get_response_code(), req.get_error_msg());
    self->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());
    return 0;
  }

  std::string http_content;
  req.get_response_stream().str().swap(http_content);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self, "Etcd cluster got http response: {}", http_content);

  do {
    // 如果lease不存在（没有TTL）则启动创建流程
    // 忽略空数据
    rapidjson::Document doc;
    if (false == atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
      break;
    }

    std::string token;
    if (false == etcd_packer::unpack_string(doc, "token", token)) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd authenticate failed, token not found.({})", http_content);
      self->add_stats_error_request();
      return 0;
    }

    self->conf_.authorization_header = "Authorization: " + token;
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self, "Etcd cluster got authenticate token: {}", token);

    self->add_stats_success_request();
    self->retry_pending_actions();

    // Renew user token later
    if (std::chrono::system_clock::duration::zero() >= self->conf_.auth_user_get_retry_interval) {
      self->conf_.auth_user_get_next_update_time = util::time::time_utility::sys_now();
    } else {
      self->conf_.auth_user_get_next_update_time =
          util::time::time_utility::sys_now() + self->conf_.auth_user_get_retry_interval;
    }
  } while (false);

  return 0;
}

bool etcd_cluster::create_request_auth_user_get() {
  if (!curl_multi_) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  if (conf_.path_node.empty()) {
    return false;
  }

  if (conf_.authorization.empty()) {
    return false;
  }

  if (util::time::time_utility::sys_now() <= conf_.auth_user_get_next_update_time) {
    return false;
  }

  if (rpc_authenticate_) {
    rpc_authenticate_->stop();
    rpc_authenticate_.reset();
  }

  if (std::chrono::system_clock::duration::zero() >= conf_.auth_user_get_retry_interval) {
    conf_.auth_user_get_next_update_time = util::time::time_utility::sys_now() + std::chrono::seconds(3);
  } else {
    conf_.auth_user_get_next_update_time = util::time::time_utility::sys_now() + conf_.auth_user_get_retry_interval;
  }

  util::network::http_request::ptr_t req = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_AUTH_USER_GET));

  std::string username;
  pick_conf_authorization(username, nullptr);

  if (req) {
    add_stats_create_request();

    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember("name", rapidjson::StringRef(username.c_str(), username.size()), doc.GetAllocator());

    setup_http_request(req, doc, get_http_timeout_ms());
    req->set_priv_data(this);
    req->set_on_complete(libcurl_callback_on_auth_user_get);

    if (get_conf_http_debug_mode()) {
      util::log::log_wrapper::ptr_t logger = logger_;
      req->set_on_verbose([logger](util::network::http_request &req, curl_infotype type, char *data, size_t size) {
        return details::etcd_cluster_verbose_callback(req, type, data, size, logger);
      });
    }
    int res = req->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      req->set_on_complete(nullptr);
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this, "Etcd start user get request for user {} to {} failed, res: {}",
                                            username, req->get_url(), res);
      add_stats_error_request();
      return false;
    }

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(*this, "Etcd start user get request for user {} to {}", username,
                                         req->get_url());
    rpc_authenticate_ = req;
  } else {
    add_stats_error_request();
  }

  return !!rpc_authenticate_;
}

int etcd_cluster::libcurl_callback_on_auth_user_get(util::network::http_request &req) {
  etcd_cluster *self = reinterpret_cast<etcd_cluster *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd user get shouldn't has request without private data");
    return 0;
  }

  util::network::http_request::ptr_t keep_rpc = self->rpc_authenticate_;
  self->rpc_authenticate_.reset();

  // 服务器错误则忽略
  if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                       util::network::http_request::get_status_code_group(req.get_response_code())) {
    // only network error will trigger a etcd member update
    if (0 != req.get_error_code()) {
      self->retry_request_member_update(req.get_url());
    }
    self->add_stats_error_request();

    if (ETCD_API_V3_ERROR_HTTP_CODE_AUTH == req.get_response_code()) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(
          *self, "Etcd user get failed with authentication code expired, we will try to obtain a new one.{}",
          req.get_error_msg());
    } else {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(*self, "Etcd user get failed, error code: {}, http code: {}\n{}",
                                           req.get_error_code(), req.get_response_code(), req.get_error_msg());
    }

    self->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());
    return 0;
  }

  std::string http_content;
  req.get_response_stream().str().swap(http_content);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self, "Etcd cluster got http response: {}", http_content);

  bool is_success = false;
  do {
    // 如果lease不存在（没有TTL）则启动创建流程
    // 忽略空数据
    rapidjson::Document doc;
    if (false == atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
      break;
    }

    std::string username;
    self->pick_conf_authorization(username, nullptr);

    rapidjson::Value::ConstMemberIterator iter = doc.FindMember("roles");
    if (iter == doc.MemberEnd() || false == iter->value.IsArray()) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self, "Etcd user get {} without any role", username);
      break;
    }

    rapidjson::Document::ConstArray roles = iter->value.GetArray();

    self->conf_.authorization_user_roles.clear();
    self->conf_.authorization_user_roles.reserve(static_cast<size_t>(roles.Size()));
    for (rapidjson::Document::Array::ConstValueIterator role_iter = roles.Begin(); role_iter != roles.End();
         ++role_iter) {
      if (role_iter->IsString()) {
        self->conf_.authorization_user_roles.push_back(role_iter->GetString());
      } else {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd user get {} with bad role type: {}", username,
                                              etcd_packer::unpack_to_string(*role_iter));
      }
    }

    std::stringstream ss;
    ss << "[ ";
    for (size_t i = 0; i < self->conf_.authorization_user_roles.size(); ++i) {
      if (0 != i) {
        ss << ", ";
      }
      ss << self->conf_.authorization_user_roles[i];
    }
    ss << " ]";
    std::string authorization_user_roles = ss.str();
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self, "Etcd cluster got user {} with roles: {}", username,
                                          authorization_user_roles);

    is_success = true;
  } while (false);

  if (is_success) {
    self->add_stats_success_request();
    self->retry_pending_actions();
  }

  return 0;
}

bool etcd_cluster::retry_request_member_update(const std::string &bad_url) {
  if (conf_.path_node.empty()) {
    select_cluster_member();
  } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(conf_.path_node.c_str(), bad_url.c_str(), conf_.path_node.size())) {
    for (size_t i = 0; i < conf_.hosts.size(); ++i) {
      if (conf_.hosts[i] != conf_.path_node) {
        continue;
      }

      if (i + 1 != conf_.hosts.size()) {
        conf_.hosts[i].swap(conf_.hosts[conf_.hosts.size() - 1]);
      }
      conf_.hosts.pop_back();
    }

    select_cluster_member();
  }

  std::chrono::system_clock::duration retry_interval = conf_.etcd_members_retry_interval;
  if (!check_flag(flag_t::RUNNING) &&
      conf_.etcd_members_init_retry_interval > std::chrono::system_clock::duration::zero()) {
    retry_interval = conf_.etcd_members_init_retry_interval;
  } else if (retry_interval <= std::chrono::system_clock::duration::zero()) {
    retry_interval = std::chrono::minutes(1);
  }
  if (util::time::time_utility::sys_now() + retry_interval < conf_.etcd_members_next_update_time) {
    conf_.etcd_members_next_update_time = util::time::time_utility::sys_now() + retry_interval;
    return false;
  }

  if (util::time::time_utility::sys_now() <= conf_.etcd_members_next_update_time) {
    return false;
  }

  return create_request_member_update();
}

bool etcd_cluster::create_request_member_update() {
  if (!curl_multi_) {
    return false;
  }

  if (check_flag(flag_t::CLOSING)) {
    return false;
  }

  if (conf_.conf_hosts.empty() && conf_.hosts.empty()) {
    return false;
  }

  if (std::chrono::system_clock::duration::zero() >= conf_.etcd_members_update_interval) {
    conf_.etcd_members_next_update_time = util::time::time_utility::sys_now() + std::chrono::minutes(1);
  } else {
    conf_.etcd_members_next_update_time = util::time::time_utility::sys_now() + conf_.etcd_members_update_interval;
  }

  if (rpc_update_members_) {
    rpc_update_members_->stop();
    rpc_update_members_.reset();
  }

  // Trust etcd ingress and do not use cluster member to keep healthy
  if (!conf_.auto_update_hosts && !conf_.conf_hosts.empty()) {
    if (conf_.hosts.empty()) {
      conf_.hosts = conf_.conf_hosts;
      select_cluster_member();
    }
    return false;
  }

  std::string *selected_host;
  if (!conf_.hosts.empty()) {
    selected_host = &conf_.hosts[random_generator_.random_between<size_t>(0, conf_.hosts.size())];
  } else {
    selected_host = &conf_.conf_hosts[random_generator_.random_between<size_t>(0, conf_.conf_hosts.size())];
  }

  util::network::http_request::ptr_t req = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", (*selected_host), ETCD_API_V3_MEMBER_LIST));

  if (req) {
    add_stats_create_request();

    rapidjson::Document doc;
    doc.SetObject();

    setup_http_request(req, doc, get_http_timeout_ms());
    req->set_priv_data(this);
    req->set_on_complete(libcurl_callback_on_member_update);

    int res = req->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      req->set_on_complete(nullptr);
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this, "Etcd start update member {} request to {} failed, res: {}",
                                            get_lease(), req->get_url().c_str(), res);

      add_stats_error_request();
      return false;
    }

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*this, "Etcd start update member {} request to {}", get_lease(),
                                          req->get_url());
    rpc_update_members_ = req;
  } else {
    add_stats_error_request();
  }

  return true;
}

int etcd_cluster::libcurl_callback_on_member_update(util::network::http_request &req) {
  etcd_cluster *self = reinterpret_cast<etcd_cluster *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd member list shouldn't has request without private data");
    return 0;
  }

  util::network::http_request::ptr_t keep_rpc = self->rpc_update_members_;
  self->rpc_update_members_.reset();

  // 服务器错误则忽略
  if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                       util::network::http_request::get_status_code_group(req.get_response_code())) {
    // only network error will trigger a etcd member update
    if (0 != req.get_error_code()) {
      self->retry_request_member_update(req.get_url());
    }
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd member list failed, error code: {}, http code: {}\n{}",
                                          req.get_error_code(), req.get_response_code(), req.get_error_msg());
    self->add_stats_error_request();

    return 0;
  }

  std::string http_content;
  req.get_response_stream().str().swap(http_content);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self, "Etcd cluster got http response: {}", http_content);

  do {
    // ignore empty data
    rapidjson::Document doc;
    if (false == atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
      break;
    }

    rapidjson::Document::ConstMemberIterator members = doc.FindMember("members");
    if (doc.MemberEnd() == members) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd members not found");
      self->add_stats_error_request();
      return 0;
    }

    if (!members->value.IsArray()) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd members is not a array");
      self->add_stats_error_request();
      return 0;
    }

    self->conf_.hosts.clear();
    bool need_select_node = true;
    rapidjson::Document::ConstArray all_members = members->value.GetArray();
    for (rapidjson::Document::Array::ConstValueIterator iter = all_members.Begin(); iter != all_members.End(); ++iter) {
      rapidjson::Document::ConstMemberIterator client_urls = iter->FindMember("clientURLs");
      if (client_urls == iter->MemberEnd()) {
        continue;
      }

      rapidjson::Document::ConstArray all_client_urls = client_urls->value.GetArray();
      for (rapidjson::Document::Array::ConstValueIterator cli_url_iter = all_client_urls.Begin();
           cli_url_iter != all_client_urls.End(); ++cli_url_iter) {
        if (cli_url_iter->GetStringLength() > 0) {
          self->conf_.hosts.push_back(cli_url_iter->GetString());

          if (self->conf_.path_node == self->conf_.hosts.back()) {
            need_select_node = false;
          }
        }
      }
    }

    if (need_select_node) {
      self->select_cluster_member();
    }

    self->add_stats_success_request();

    if (std::chrono::system_clock::duration::zero() >= self->conf_.etcd_members_update_interval) {
      self->conf_.etcd_members_next_update_time = util::time::time_utility::sys_now() + std::chrono::minutes(1);
    } else {
      self->conf_.etcd_members_next_update_time =
          util::time::time_utility::sys_now() + self->conf_.etcd_members_update_interval;
    }

    // 触发一次tick
    self->tick();
  } while (false);

  return 0;
}

bool etcd_cluster::create_request_lease_grant() {
  if (!curl_multi_ || conf_.path_node.empty()) {
    return false;
  }

  if (check_flag(flag_t::CLOSING) || !check_flag(flag_t::ENABLE_LEASE)) {
    return false;
  }

  if (util::time::time_utility::sys_now() <= conf_.keepalive_next_update_time) {
    return false;
  }

  if (rpc_keepalive_) {
    rpc_keepalive_->stop();
    rpc_keepalive_.reset();
  }

  if (check_flag(flag_t::RUNNING) && conf_.keepalive_interval > std::chrono::system_clock::duration::zero()) {
    conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + conf_.keepalive_interval;
  } else if (conf_.keepalive_retry_interval > std::chrono::system_clock::duration::zero()) {
    conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + conf_.keepalive_retry_interval;
  } else {
    conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + std::chrono::seconds(3);
  }

  util::network::http_request::ptr_t req = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_LEASE_GRANT));

  if (req) {
    add_stats_create_request();

    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember("ID", get_lease(), doc.GetAllocator());
    doc.AddMember(
        "TTL", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(conf_.keepalive_timeout).count()),
        doc.GetAllocator());

    setup_http_request(req, doc, get_http_timeout_ms());
    req->set_priv_data(this);
    req->set_on_complete(libcurl_callback_on_lease_keepalive);

    if (get_conf_http_debug_mode()) {
      util::log::log_wrapper::ptr_t logger = logger_;
      req->set_on_verbose([logger](util::network::http_request &req, curl_infotype type, char *data, size_t size) {
        return details::etcd_cluster_verbose_callback(req, type, data, size, logger);
      });
    }
    int res = req->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      req->set_on_complete(nullptr);
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this, "Etcd start keepalive lease {} request to {} failed, res: {}",
                                            get_lease(), req->get_url(), res);
      add_stats_error_request();
      return false;
    }

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*this, "Etcd start keepalive lease {} request to {}", get_lease(),
                                          req->get_url());
    rpc_keepalive_ = req;
  } else {
    add_stats_error_request();
    if (std::chrono::system_clock::duration::zero() >= conf_.keepalive_retry_interval) {
      conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + conf_.keepalive_retry_interval;
    }
  }

  return true;
}

bool etcd_cluster::create_request_lease_keepalive() {
  if (!curl_multi_ || 0 == get_lease() || conf_.path_node.empty()) {
    return false;
  }

  if (check_flag(flag_t::CLOSING) || !check_flag(flag_t::ENABLE_LEASE)) {
    return false;
  }

  if (util::time::time_utility::sys_now() <= conf_.keepalive_next_update_time) {
    return false;
  }

  if (rpc_keepalive_) {
    rpc_keepalive_->stop();
    rpc_keepalive_.reset();
  }

  if (std::chrono::system_clock::duration::zero() >= conf_.keepalive_interval) {
    conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + std::chrono::seconds(3);
  } else {
    conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + conf_.keepalive_interval;
  }

  util::network::http_request::ptr_t req = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_LEASE_KEEPALIVE));

  if (req) {
    add_stats_create_request();

    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember("ID", get_lease(), doc.GetAllocator());

    setup_http_request(req, doc, get_http_timeout_ms());
    req->set_priv_data(this);
    req->set_on_complete(libcurl_callback_on_lease_keepalive);

    int res = req->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      req->set_on_complete(nullptr);
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*this, "Etcd start keepalive lease {} request to {} failed, res: {}",
                                            get_lease(), req->get_url().c_str(), res);
      add_stats_error_request();
      return false;
    }

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*this, "Etcd start keepalive lease {} request to {}", get_lease(),
                                          req->get_url());
    rpc_keepalive_ = req;
  } else {
    add_stats_error_request();
    if (std::chrono::system_clock::duration::zero() >= conf_.keepalive_retry_interval) {
      conf_.keepalive_next_update_time = util::time::time_utility::sys_now() + conf_.keepalive_retry_interval;
    }
  }

  return true;
}

int etcd_cluster::libcurl_callback_on_lease_keepalive(util::network::http_request &req) {
  etcd_cluster *self = reinterpret_cast<etcd_cluster *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd lease keepalive shouldn't has request without private data");
    return 0;
  }

  util::network::http_request::ptr_t keep_rpc = self->rpc_keepalive_;
  self->rpc_keepalive_.reset();

  // 服务器错误则忽略
  if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                       util::network::http_request::get_status_code_group(req.get_response_code())) {
    // only network error will trigger a etcd member update
    if (0 != req.get_error_code()) {
      self->retry_request_member_update(req.get_url());
    }
    self->add_stats_error_request();

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd lease keepalive failed, error code: {}, http code: {}\n{}",
                                          req.get_error_code(), req.get_response_code(), req.get_error_msg());
    self->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());
    return 0;
  }

  std::string http_content;
  req.get_response_stream().str().swap(http_content);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self, "Etcd cluster got http response: {}", http_content);

  do {
    // 如果lease不存在（没有TTL）则启动创建流程
    // 忽略空数据
    rapidjson::Document doc;
    if (false == atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
      break;
    }

    bool is_grant = false;
    const rapidjson::Value *root = &doc;
    rapidjson::Value::ConstMemberIterator result = doc.FindMember("result");
    if (result == doc.MemberEnd()) {
      is_grant = true;
    } else {
      root = &result->value;
    }

    if (root->MemberEnd() == root->FindMember("TTL")) {
      if (is_grant) {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd lease grant failed");
      } else {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self, "Etcd lease keepalive failed because not found, try to grant one");
        self->set_lease(0, is_grant);
        self->create_request_lease_grant();
      }

      self->add_stats_error_request();
      return 0;
    }

    // 更新lease
    int64_t new_lease = 0;
    etcd_packer::unpack_int(*root, "ID", new_lease);

    if (0 == new_lease) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
          *self, "Etcd cluster got a error http response for grant or keepalive lease: {}", http_content);
      self->add_stats_error_request();
      break;
    }

    if (is_grant) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(*self, "Etcd lease {} granted", new_lease);
      if (self->logger_) {
        FWINSTLOGINFO(*self->logger_, "Etcd lease {} keepalive successed", new_lease);
      }
    } else {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(*self, "Etcd lease {} keepalive successed", new_lease);
    }

    self->add_stats_success_request();
    if (!self->check_flag(flag_t::RUNNING) && !self->check_flag(flag_t::CLOSING)) {
      self->set_flag(flag_t::RUNNING, true);
    }

    // 这里会触发重试失败的keepalive
    self->set_lease(new_lease, is_grant);
  } while (false);

  return 0;
}

util::network::http_request::ptr_t etcd_cluster::create_request_lease_revoke() {
  if (!curl_multi_ || 0 == get_lease() || conf_.path_node.empty()) {
    return util::network::http_request::ptr_t();
  }

  util::network::http_request::ptr_t ret = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_LEASE_REVOKE));

  if (ret) {
    add_stats_create_request();

    rapidjson::Document doc;
    doc.SetObject();
    doc.AddMember("ID", get_lease(), doc.GetAllocator());

    setup_http_request(ret, doc, get_http_timeout_ms());
  } else {
    add_stats_error_request();
  }

  return ret;
}

LIBATAPP_MACRO_API util::network::http_request::ptr_t etcd_cluster::create_request_kv_get(const std::string &key,
                                                                                          const std::string &range_end,
                                                                                          int64_t limit,
                                                                                          int64_t revision) {
  if (!curl_multi_ || conf_.path_node.empty() || check_flag(flag_t::CLOSING)) {
    return util::network::http_request::ptr_t();
  }

  util::network::http_request::ptr_t ret = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_KV_GET));

  if (ret) {
    add_stats_create_request();

    rapidjson::Document doc;
    rapidjson::Value &root = doc.SetObject();

    etcd_packer::pack_key_range(root, key, range_end, doc);
    doc.AddMember("limit", limit, doc.GetAllocator());
    doc.AddMember("revision", revision, doc.GetAllocator());

    setup_http_request(ret, doc, get_http_timeout_ms());
  } else {
    add_stats_error_request();
  }

  return ret;
}

LIBATAPP_MACRO_API util::network::http_request::ptr_t etcd_cluster::create_request_kv_set(
    const std::string &key, const std::string &value, bool assign_lease, bool prev_kv, bool ignore_value,
    bool ignore_lease) {
  if (!curl_multi_ || conf_.path_node.empty() || check_flag(flag_t::CLOSING)) {
    return util::network::http_request::ptr_t();
  }

  if (assign_lease && 0 == get_lease()) {
    return util::network::http_request::ptr_t();
  }

  util::network::http_request::ptr_t ret = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_KV_SET));

  if (ret) {
    add_stats_create_request();

    rapidjson::Document doc;
    rapidjson::Value &root = doc.SetObject();

    etcd_packer::pack_base64(root, "key", key, doc);
    etcd_packer::pack_base64(root, "value", value, doc);
    if (assign_lease) {
      doc.AddMember("lease", get_lease(), doc.GetAllocator());
    }

    doc.AddMember("prev_kv", prev_kv, doc.GetAllocator());
    doc.AddMember("ignore_value", ignore_value, doc.GetAllocator());
    doc.AddMember("ignore_lease", ignore_lease, doc.GetAllocator());

    setup_http_request(ret, doc, get_http_timeout_ms());
  } else {
    add_stats_error_request();
  }

  return ret;
}

LIBATAPP_MACRO_API util::network::http_request::ptr_t etcd_cluster::create_request_kv_del(const std::string &key,
                                                                                          const std::string &range_end,
                                                                                          bool prev_kv) {
  if (!curl_multi_ || conf_.path_node.empty() || check_flag(flag_t::CLOSING)) {
    return util::network::http_request::ptr_t();
  }

  util::network::http_request::ptr_t ret = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_KV_DELETE));

  if (ret) {
    add_stats_create_request();

    rapidjson::Document doc;
    rapidjson::Value &root = doc.SetObject();

    etcd_packer::pack_key_range(root, key, range_end, doc);
    doc.AddMember("prev_kv", prev_kv, doc.GetAllocator());

    setup_http_request(ret, doc, get_http_timeout_ms());
  } else {
    add_stats_error_request();
  }

  return ret;
}

LIBATAPP_MACRO_API util::network::http_request::ptr_t etcd_cluster::create_request_watch(
    const std::string &key, const std::string &range_end, int64_t start_revision, bool prev_kv, bool progress_notify) {
  if (!curl_multi_ || conf_.path_node.empty() || check_flag(flag_t::CLOSING)) {
    return util::network::http_request::ptr_t();
  }

  util::network::http_request::ptr_t ret = util::network::http_request::create(
      curl_multi_.get(), LOG_WRAPPER_FWAPI_FORMAT("{}{}", conf_.path_node, ETCD_API_V3_WATCH));

  if (ret) {
    add_stats_create_request();

    rapidjson::Document doc;
    rapidjson::Value &root = doc.SetObject();

    rapidjson::Value create_request(rapidjson::kObjectType);

    etcd_packer::pack_key_range(create_request, key, range_end, doc);
    if (prev_kv) {
      create_request.AddMember("prev_kv", prev_kv, doc.GetAllocator());
    }

    create_request.AddMember("progress_notify", progress_notify, doc.GetAllocator());

    if (0 != start_revision) {
      create_request.AddMember("start_revision", start_revision, doc.GetAllocator());
    }

    root.AddMember("create_request", create_request, doc.GetAllocator());

    setup_http_request(ret, doc, get_http_timeout_ms());
    ret->set_opt_keepalive(75, 150);
    // 不能共享socket
    ret->set_opt_reuse_connection(false);
  } else {
    add_stats_error_request();
  }

  return ret;
}

LIBATAPP_MACRO_API etcd_cluster::on_event_up_down_handle_t etcd_cluster::add_on_event_up(on_event_up_down_fn_t fn,
                                                                                         bool trigger_if_running) {
  if (!fn) {
    return event_on_up_callbacks_.end();
  }

  if (trigger_if_running && check_flag(flag_t::RUNNING)) {
    fn(*this);
  }

  return event_on_up_callbacks_.insert(event_on_up_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void etcd_cluster::remove_on_event_up(on_event_up_down_handle_t &handle) {
  if (handle == event_on_up_callbacks_.end()) {
    return;
  }

  event_on_up_callbacks_.erase(handle);
  handle = event_on_up_callbacks_.end();
}

LIBATAPP_MACRO_API void etcd_cluster::reset_on_event_up_handle(on_event_up_down_handle_t &handle) {
  handle = event_on_up_callbacks_.end();
}

LIBATAPP_MACRO_API etcd_cluster::on_event_up_down_handle_t etcd_cluster::add_on_event_down(
    on_event_up_down_fn_t fn, bool trigger_if_not_running) {
  if (!fn) {
    return event_on_down_callbacks_.end();
  }

  if (trigger_if_not_running && !check_flag(flag_t::RUNNING)) {
    fn(*this);
  }

  return event_on_down_callbacks_.insert(event_on_down_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void etcd_cluster::remove_on_event_down(on_event_up_down_handle_t &handle) {
  if (handle == event_on_down_callbacks_.end()) {
    return;
  }

  event_on_down_callbacks_.erase(handle);
  handle = event_on_down_callbacks_.end();
}

LIBATAPP_MACRO_API void etcd_cluster::reset_on_event_down_handle(on_event_up_down_handle_t &handle) {
  handle = event_on_down_callbacks_.end();
}

void etcd_cluster::add_stats_error_request() {
  ++stats_.sum_error_requests;
  ++stats_.continue_error_requests;
  stats_.continue_success_requests = 0;
}

void etcd_cluster::add_stats_success_request() {
  ++stats_.sum_success_requests;
  ++stats_.continue_success_requests;
  stats_.continue_error_requests = 0;
}

void etcd_cluster::add_stats_create_request() { ++stats_.sum_create_requests; }

bool etcd_cluster::check_authorization() const {
  if (conf_.authorization.empty()) {
    return true;
  }

  if (!conf_.authorization_header.empty()) {
    return true;
  }

  return false;
}

void etcd_cluster::delete_keepalive_deletor(etcd_keepalive_deletor *in, bool close_rpc) {
  if (nullptr == in) {
    return;
  }

  in->owner = nullptr;
  if (close_rpc) {
    if (in->rpc) {
      in->rpc->set_priv_data(nullptr);
      in->rpc->set_on_complete(nullptr);
      in->rpc->stop();
    }
  } else {
    if (in->rpc) {
      in->rpc->set_priv_data(nullptr);
    }
  }

  delete in;
}

void etcd_cluster::cleanup_keepalive_deletors() {
  while (!keepalive_deletors_.empty()) {
    etcd_keepalive_deletor_map_t pending_deletes;
    pending_deletes.swap(keepalive_deletors_);

    for (etcd_keepalive_deletor_map_t::iterator iter = pending_deletes.begin(); iter != pending_deletes.end(); ++iter) {
      delete_keepalive_deletor(iter->second, true);
    }
  }
}

bool etcd_cluster::select_cluster_member() {
  if (!conf_.hosts.empty()) {
    if (1 == conf_.hosts.size()) {
      conf_.path_node = conf_.hosts[0];
    } else {
      conf_.path_node = conf_.hosts[random_generator_.random_between<size_t>(0, conf_.hosts.size())];
    }

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(*this, "Etcd cluster {} using node {}", reinterpret_cast<const void *>(this),
                                         conf_.path_node);
  } else {
    conf_.path_node.clear();
  }

  return !conf_.path_node.empty();
}

LIBATAPP_MACRO_API void etcd_cluster::check_authorization_expired(int http_code, const std::string &content) {
  if (ETCD_API_V3_ERROR_HTTP_CODE_AUTH == http_code) {
    conf_.authorization_header.clear();
    return;
  }

  rapidjson::Document doc;
  if (atapp::etcd_packer::parse_object(doc, content.c_str())) {
    int64_t error_code = 0;
    atapp::etcd_packer::unpack_int(doc, "code", error_code);
    if (ETCD_API_V3_ERROR_GRPC_CODE_UNAUTHENTICATED == error_code) {
      conf_.authorization_header.clear();
      return;
    }
  }

  if (ETCD_API_V3_ERROR_HTTP_INVALID_PARAM == http_code || ETCD_API_V3_ERROR_HTTP_PRECONDITION == http_code) {
    if (std::string::npos != content.find("authenticat")) {
      conf_.authorization_header.clear();
    }
  }
}

LIBATAPP_MACRO_API void etcd_cluster::setup_http_request(util::network::http_request::ptr_t &req,
                                                         rapidjson::Document &doc, time_t timeout) {
  if (!req) {
    return;
  }

  if (timeout <= 0) {
    timeout = get_http_timeout_ms();
  }

  time_t connect_timeout = get_http_connect_timeout_ms();
  if (connect_timeout > 0) {
    req->set_opt_connect_timeout(timeout);
  }
  if (get_conf_dns_cache_timeout() > std::chrono::system_clock::duration::zero()) {
    time_t dns_cache_timeout = static_cast<time_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(get_conf_dns_cache_timeout()).count());
    if (dns_cache_timeout > 0) {
      req->set_opt_long(CURLOPT_DNS_CACHE_TIMEOUT, dns_cache_timeout);
    }
  }
#if LIBCURL_VERSION_NUM >= 0x071800
  if (!get_conf_dns_servers().empty()) {
    req->set_opt_string(CURLOPT_DNS_SERVERS, get_conf_dns_servers().c_str());
  }
#endif

  req->set_opt_follow_location(true);
  req->set_opt_ssl_verify_peer(conf_.ssl_verify_peer);
  req->set_opt_long(CURLOPT_SSL_VERIFYHOST, conf_.ssl_verify_peer ? 2L : 0L);
#if LIBCURL_VERSION_NUM >= 0x072900
  req->set_opt_bool(CURLOPT_SSL_VERIFYSTATUS, conf_.ssl_verify_peer);
#endif
#if LIBCURL_VERSION_NUM >= 0x073400
  req->set_opt_bool(CURLOPT_PROXY_SSL_VERIFYPEER, conf_.ssl_verify_peer);
  req->set_opt_long(CURLOPT_PROXY_SSL_VERIFYHOST, conf_.ssl_verify_peer ? 2L : 0L);
#endif
  req->set_opt_accept_encoding("");
  req->set_opt_http_content_decoding(true);
  req->set_opt_timeout(timeout);
  if (conf_.user_agent.empty()) {
    req->set_user_agent(details::get_default_user_agent());
  } else {
    req->set_user_agent(conf_.user_agent);
  }
  // req->set_opt_reuse_connection(false); // just enable connection reuse for all but watch request
  req->set_opt_long(CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  req->set_opt_no_signal(true);
  if (!conf_.authorization_header.empty()) {
    req->append_http_header(conf_.authorization_header.c_str());
  }

  // Setup ssl
  if (ssl_version_t::DISABLED != conf_.ssl_min_version) {
#if LIBCURL_VERSION_NUM >= 0x072400
    req->set_opt_bool(CURLOPT_SSL_ENABLE_ALPN, conf_.ssl_enable_alpn);
#endif
    switch (conf_.ssl_min_version) {
#if LIBCURL_VERSION_NUM >= 0x073400
      case ssl_version_t::TLS_V13:
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3);
        req->set_opt_long(CURLOPT_PROXY_SSLVERSION, CURL_SSLVERSION_TLSv1_3);
        break;
#endif
#if LIBCURL_VERSION_NUM >= 0x072200
      case ssl_version_t::TLS_V12:
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#  if LIBCURL_VERSION_NUM >= 0x073400
        req->set_opt_long(CURLOPT_PROXY_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#  endif
        break;
      case ssl_version_t::TLS_V11:
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_1);
#  if LIBCURL_VERSION_NUM >= 0x073400
        req->set_opt_long(CURLOPT_PROXY_SSLVERSION, CURL_SSLVERSION_TLSv1_1);
#  endif
        break;
      case ssl_version_t::TLS_V10:
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1);
#  if LIBCURL_VERSION_NUM >= 0x073400
        req->set_opt_long(CURLOPT_PROXY_SSLVERSION, CURL_SSLVERSION_TLSv1);
#  endif
        break;
#endif
      case ssl_version_t::SSL3:
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_SSLv3);
        break;
      default:
#if LIBCURL_VERSION_NUM >= 0x072200
        conf_.ssl_min_version = ssl_version_t::TLS_V12;
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#  if LIBCURL_VERSION_NUM >= 0x073400
        req->set_opt_long(CURLOPT_PROXY_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#  endif
#else
        conf_.ssl_min_version = ssl_version_t::SSL3;
        req->set_opt_long(CURLOPT_SSLVERSION, CURL_SSLVERSION_SSLv3);
#endif
        break;
    }

    if (!conf_.proxy.empty()) {
      req->set_opt_string(CURLOPT_PROXY, &conf_.proxy[0]);
      if (0 == UTIL_STRFUNC_STRNCASE_CMP("http:", &conf_.proxy[0], 5) ||
          0 == UTIL_STRFUNC_STRNCASE_CMP("https:", &conf_.proxy[0], 6)) {
        req->set_opt_bool(CURLOPT_HTTPPROXYTUNNEL, true);
      }
    }

#if LIBCURL_VERSION_NUM >= 0x071304
    if (!conf_.no_proxy.empty()) {
      req->set_opt_string(CURLOPT_NOPROXY, &conf_.no_proxy[0]);
    }
#endif

#if LIBCURL_VERSION_NUM >= 0x071301
    if (!conf_.proxy_user_name.empty()) {
      req->set_opt_string(CURLOPT_PROXYUSERNAME, &conf_.proxy_user_name[0]);
    }

    if (!conf_.proxy_password.empty()) {
      req->set_opt_string(CURLOPT_PROXYPASSWORD, &conf_.proxy_password[0]);
    }
#endif

    // client cert and key
#if LIBCURL_VERSION_NUM >= 0x072400
    req->set_opt_bool(CURLOPT_SSL_ENABLE_ALPN, conf_.ssl_enable_alpn);
#endif
    if (!conf_.ssl_client_cert.empty()) {
      req->set_opt_string(CURLOPT_SSLCERT, &conf_.ssl_client_cert[0]);
    }
#if LIBCURL_VERSION_NUM >= 0x070903
    if (!conf_.ssl_client_cert_type.empty()) {
      req->set_opt_string(CURLOPT_SSLCERTTYPE, &conf_.ssl_client_cert_type[0]);
    }
#endif
    if (!conf_.ssl_client_key.empty()) {
      req->set_opt_string(CURLOPT_SSLKEY, &conf_.ssl_client_key[0]);
    }
    if (!conf_.ssl_client_key_type.empty()) {
      req->set_opt_string(CURLOPT_SSLKEYTYPE, &conf_.ssl_client_key_type[0]);
    }
#if LIBCURL_VERSION_NUM >= 0x071004
    if (!conf_.ssl_client_key_passwd.empty()) {
      req->set_opt_string(CURLOPT_SSLKEYPASSWD, &conf_.ssl_client_key_passwd[0]);
    }
#elif LIBCURL_VERSION_NUM >= 0x070902
    if (!conf_.ssl_client_key_passwd.empty()) {
      req->set_opt_string(CURLOPT_SSLCERTPASSWD, &conf_.ssl_client_key_passwd[0]);
    }
#endif
    if (!conf_.ssl_ca_cert.empty()) {
      req->set_opt_string(CURLOPT_CAINFO, &conf_.ssl_ca_cert[0]);
    }

#if LIBCURL_VERSION_NUM >= 0x071504
    if (!conf_.ssl_client_tlsauth_username.empty()) {
      req->set_opt_string(CURLOPT_TLSAUTH_TYPE,
                          "SRP");  // @see https://curl.haxx.se/libcurl/c/CURLOPT_TLSAUTH_TYPE.html
      req->set_opt_string(CURLOPT_TLSAUTH_USERNAME, &conf_.ssl_client_tlsauth_username[0]);
      req->set_opt_string(CURLOPT_TLSAUTH_PASSWORD, &conf_.ssl_client_tlsauth_password[0]);
    }
#endif
    // proxy cert and key

#if LIBCURL_VERSION_NUM >= 0x072400
    req->set_opt_bool(CURLOPT_SSL_ENABLE_ALPN, conf_.ssl_enable_alpn);
#endif

#if LIBCURL_VERSION_NUM >= 0x073400
    if (!conf_.ssl_proxy_cert.empty()) {
      req->set_opt_string(CURLOPT_PROXY_SSLCERT, &conf_.ssl_proxy_cert[0]);
    }

    if (!conf_.ssl_proxy_cert_type.empty()) {
      req->set_opt_string(CURLOPT_PROXY_SSLCERTTYPE, &conf_.ssl_proxy_cert_type[0]);
    }

    if (!conf_.ssl_proxy_key.empty()) {
      req->set_opt_string(CURLOPT_PROXY_SSLKEY, &conf_.ssl_proxy_key[0]);
    }

    if (!conf_.ssl_proxy_key_type.empty()) {
      req->set_opt_string(CURLOPT_PROXY_SSLKEYTYPE, &conf_.ssl_proxy_key_type[0]);
    }

    if (!conf_.ssl_proxy_key_passwd.empty()) {
      req->set_opt_string(CURLOPT_PROXY_KEYPASSWD, &conf_.ssl_proxy_key_passwd[0]);
    }

    if (!conf_.ssl_proxy_ca_cert.empty()) {
      req->set_opt_string(CURLOPT_PROXY_CAINFO, &conf_.ssl_proxy_ca_cert[0]);
    }

    if (!conf_.ssl_proxy_tlsauth_username.empty()) {
      req->set_opt_string(CURLOPT_PROXY_TLSAUTH_TYPE,
                          "SRP");  // @see https://curl.haxx.se/libcurl/c/CURLOPT_PROXY_TLSAUTH_TYPE.html
      req->set_opt_string(CURLOPT_PROXY_TLSAUTH_USERNAME, &conf_.ssl_proxy_tlsauth_username[0]);
      req->set_opt_string(CURLOPT_PROXY_TLSAUTH_PASSWORD, &conf_.ssl_proxy_tlsauth_password[0]);
    }
#endif
    // ssl cipher
    if (!conf_.ssl_cipher_list.empty()) {
      req->set_opt_string(CURLOPT_SSL_CIPHER_LIST, &conf_.ssl_cipher_list[0]);
    }

#if LIBCURL_VERSION_NUM >= 0x073d00
    if (!conf_.ssl_cipher_list_tls13.empty()) {
      req->set_opt_string(CURLOPT_TLS13_CIPHERS, &conf_.ssl_cipher_list_tls13[0]);
    }
#endif
  }

  if (get_conf_http_debug_mode()) {
    util::log::log_wrapper::ptr_t logger = logger_;
    req->set_on_verbose([logger](util::network::http_request &req, curl_infotype type, char *data, size_t size) {
      return details::etcd_cluster_verbose_callback(req, type, data, size, logger);
    });
  }

  if (util::log::log_wrapper::check_level(WDTLOGGETCAT(util::log::log_wrapper::categorize_t::DEFAULT),
                                          util::log::log_wrapper::level_t::LOG_LW_TRACE) ||
      (logger_ && logger_->check_level(util::log::log_wrapper::level_t::LOG_LW_TRACE))) {
    auto logger = logger_;
    req->set_on_progress(
        [logger](util::network::http_request &req, const util::network::http_request::progress_t &process) {
          FWLOGTRACE("Etcd cluster {} http request {} to {}, process: download {}/{}, upload {}/{}",
                     req.get_priv_data(), reinterpret_cast<const void *>(&req), req.get_url(), process.dlnow,
                     process.dltotal, process.ulnow, process.ultotal);
          if (logger) {
            FWINSTLOGTRACE(*logger, "Etcd cluster {} http request {} to {}, process: download {}/{}, upload {}/{}",
                           req.get_priv_data(), reinterpret_cast<const void *>(&req), req.get_url(), process.dlnow,
                           process.dltotal, process.ulnow, process.ultotal);
          }
          return 0;
        });
  }

  // Stringify the DOM
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  req->post_data().assign(buffer.GetString(), buffer.GetSize());
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*this, "Etcd cluster setup request {} to {}, post data: {}",
                                        reinterpret_cast<const void *>(req.get()), req->get_url(), req->post_data());
}
}  // namespace atapp
