// Copyright 2021 atframework
// Created by owent on 2017-11-17

#pragma once

#include <log/log_wrapper.h>
#include <network/http_request.h>
#include <random/random_generator.h>
#include <time/time_utility.h>

#include <detail/libatbus_config.h>

#include <chrono>
#include <ctime>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "atframe/etcdcli/etcd_packer.h"

namespace atapp {
class etcd_keepalive;
class etcd_watcher;
class etcd_cluster;

struct etcd_keepalive_deletor {
  util::network::http_request::ptr_t rpc;
  std::string path;
  void *keepalive_addr;  // maybe already destroyed, just used to write log
  size_t retry_times;
  etcd_cluster *owner;
};

class etcd_cluster {
 public:
  struct LIBATAPP_MACRO_API_HEAD_ONLY flag_t {
    enum type {
      CLOSING = 0x0001,  // closeing
      RUNNING = 0x0002,
      READY = 0x0004,                     // ready
      ENABLE_LEASE = 0x0100,              // enable auto get lease
      PREVIOUS_REQUEST_TIMEOUT = 0x0200,  // if previous request timeout, then do not reuse socket
    };
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY ssl_version_t {
    enum type {
      DISABLED = 0,
      SSL3,     // default for curl version < 7.34.0
      TLS_V10,  // TLSv1.0
      TLS_V11,  // TLSv1.1
      TLS_V12,  // TLSv1.2, default for curl version >= 7.34.0
      TLS_V13,  // TLSv1.3
    };
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY conf_t {
    std::vector<std::string> conf_hosts;
    std::vector<std::string> hosts;
    std::string authorization;
    std::chrono::system_clock::duration http_request_timeout;
    std::chrono::system_clock::duration http_initialization_timeout;
    std::chrono::system_clock::duration http_connect_timeout;
    std::chrono::system_clock::duration dns_cache_timeout;
    std::string dns_servers;

    // generated data for cluster members
    std::vector<std::string> authorization_user_roles;
    std::string authorization_header;
    std::chrono::system_clock::time_point authorization_next_update_time;
    std::chrono::system_clock::duration authorization_retry_interval;
    /**
     * @note We use /v3/auth/user/get to renew the timeout of authorization token.
     *       /v3/lease/keepalive do not need authorization and can not used to renew the token.
     *       auth-token=simple will set timeout to 5 minutes, so we use auth_user_get_retry_interval=2 minutes for
     * default If set auth-token=jwt,pub-key=PUBLIC KEY,priv-key=PRIVITE KEY,sign-method=METHOD,ttl=TTL please set
     * auth_user_get_retry_interval to any value less than TTL
     * @see https://etcd.io/docs/v3.4.0/op-guide/configuration/#--auth-token
     * @see https://github.com/etcd-io/etcd/blob/master/auth/simple_token.go
     * @see https://github.com/etcd-io/etcd/blob/master/auth/jwt.go
     */
    std::chrono::system_clock::time_point auth_user_get_next_update_time;
    std::chrono::system_clock::duration auth_user_get_retry_interval;
    std::string path_node;
    std::chrono::system_clock::time_point etcd_members_next_update_time;
    std::chrono::system_clock::duration etcd_members_update_interval;
    std::chrono::system_clock::duration etcd_members_retry_interval;
    std::chrono::system_clock::duration etcd_members_init_retry_interval;

    // generated data for lease
    int64_t lease;
    std::chrono::system_clock::time_point keepalive_next_update_time;
    std::chrono::system_clock::duration keepalive_timeout;
    std::chrono::system_clock::duration keepalive_interval;
    std::chrono::system_clock::duration keepalive_retry_interval;
    size_t keepalive_retry_times;

    // SSL configure
    // @see https://github.com/etcd-io/etcd/blob/master/Documentation/op-guide/security.md for detail
    bool ssl_enable_alpn;    // curl 7.36.0 CURLOPT_SSL_ENABLE_ALPN
    bool ssl_verify_peer;    // CURLOPT_SSL_VERIFYPEER, CURLOPT_SSL_VERIFYHOST, CURLOPT_SSL_VERIFYSTATUS and
                             // CURLOPT_PROXY_SSL_VERIFYPEER, CURLOPT_PROXY_SSL_VERIFYHOST
    bool http_debug_mode;    // print verbose information
    bool auto_update_hosts;  // auto update cluster member

    ssl_version_t::type ssl_min_version;  // CURLOPT_SSLVERSION and CURLOPT_PROXY_SSLVERSION @see ssl_version_t,
                                          // SSLv3/TLSv1/TLSv1.1/TLSv1.2/TLSv1.3
    std::string user_agent;               // CURLOPT_USERAGENT
    std::string proxy;            // CURLOPT_HTTPPROXYTUNNEL, CURLOPT_PROXY: [SCHEME]://HOST[:PORT], SCHEME is one of
                                  // http,https,socks4,socks4a,socks5,socks5h. PORT's default value is 1080
    std::string no_proxy;         // curl 7.19.4 CURLOPT_NOPROXY
    std::string proxy_user_name;  // curl 7.19.1 CURLOPT_PROXYUSERNAME
    std::string proxy_password;   // curl 7.19.1 CURLOPT_PROXYPASSWORD

    std::string ssl_client_cert;              // CURLOPT_SSLCERT
    std::string ssl_client_cert_type;         // curl 7.9.3 CURLOPT_SSLCERTTYPE, PEM or DER, PEM for default
    std::string ssl_client_key;               // CURLOPT_SSLKEY
    std::string ssl_client_key_type;          // CURLOPT_SSLKEYTYPE: PEM, DER or ENG
    std::string ssl_client_key_passwd;        // curl 7.16.4 CURLOPT_SSLKEYPASSWD , curl 7.9.2 CURLOPT_SSLCERTPASSWD
    std::string ssl_ca_cert;                  // CURLOPT_CAINFO
    std::string ssl_client_tlsauth_username;  // CURLOPT_TLSAUTH_USERNAME
    std::string ssl_client_tlsauth_password;  // CURLOPT_TLSAUTH_PASSWORD

    std::string ssl_proxy_cert;        // CURLOPT_PROXY_SSLCERT
    std::string ssl_proxy_cert_type;   // curl 7.9.3 CURLOPT_PROXY_SSLCERTTYPE, PEM or DER, PEM for default
    std::string ssl_proxy_key;         // CURLOPT_PROXY_SSLKEY
    std::string ssl_proxy_key_type;    // CURLOPT_PROXY_SSLKEYTYPE: PEM, DER or ENG
    std::string ssl_proxy_key_passwd;  // curl 7.16.4 CURLOPT_PROXY_KEYPASSWD , curl 7.9.2 CURLOPT_PROXY_SSLCERTPASSWD
    std::string ssl_proxy_ca_cert;     // CURLOPT_PROXY_CAINFO
    std::string ssl_proxy_tlsauth_username;  // CURLOPT_PROXY_TLSAUTH_USERNAME
    std::string ssl_proxy_tlsauth_password;  // CURLOPT_PROXY_TLSAUTH_PASSWORD

    std::string ssl_cipher_list;        // CURLOPT_SSL_CIPHER_LIST, @see https://curl.haxx.se/docs/ssl-ciphers.html
    std::string ssl_cipher_list_tls13;  // curl 7.61.0, openssl 1.1.1 CURLOPT_TLS13_CIPHERS, default:
                                        //   TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256
  };

  struct LIBATAPP_MACRO_API_HEAD_ONLY stats_t {
    size_t sum_error_requests;
    size_t continue_error_requests;
    size_t sum_success_requests;
    size_t continue_success_requests;

    size_t sum_create_requests;
  };

  using on_event_up_down_fn_t = std::function<void(etcd_cluster &)>;
  using on_event_up_down_handle_set_t = std::list<on_event_up_down_fn_t>;
  using on_event_up_down_handle_t = on_event_up_down_handle_set_t::iterator;

 public:
  LIBATAPP_MACRO_API etcd_cluster();
  LIBATAPP_MACRO_API ~etcd_cluster();

  LIBATAPP_MACRO_API void init(const util::network::http_request::curl_m_bind_ptr_t &curl_mgr);

  LIBATAPP_MACRO_API util::network::http_request::ptr_t close(bool wait, bool revoke_lease);
  LIBATAPP_MACRO_API void reset();
  LIBATAPP_MACRO_API int tick();
  LIBATAPP_MACRO_API bool is_available() const;
  LIBATAPP_MACRO_API void resolve_ready() noexcept;

  UTIL_FORCEINLINE bool check_flag(uint32_t f) const { return 0 != (flags_ & f); };
  LIBATAPP_MACRO_API void set_flag(flag_t::type f, bool v);

  UTIL_FORCEINLINE const stats_t &get_stats() const { return stats_; };

  LIBATAPP_MACRO_API void set_logger(const util::log::log_wrapper::ptr_t &logger,
                                     util::log::log_formatter::level_t::type log_level) noexcept;
  UTIL_FORCEINLINE const util::log::log_wrapper::ptr_t &get_logger() const noexcept { return logger_; }

  // ====================== apis for configure ==================
  UTIL_FORCEINLINE const std::vector<std::string> &get_available_hosts() const { return conf_.hosts; }
  UTIL_FORCEINLINE const std::string &get_selected_host() const { return conf_.path_node; }

  UTIL_FORCEINLINE void set_conf_authorization(const std::string &authorization) {
    conf_.authorization = authorization;
  }
  UTIL_FORCEINLINE const std::string &get_conf_authorization() const { return conf_.authorization; }
  LIBATAPP_MACRO_API void pick_conf_authorization(std::string &username, std::string *password);

  UTIL_FORCEINLINE int64_t get_keepalive_lease() const { return get_lease(); }

  UTIL_FORCEINLINE void set_conf_hosts(const std::vector<std::string> &hosts) { conf_.conf_hosts = hosts; }
  UTIL_FORCEINLINE const std::vector<std::string> &get_conf_hosts() const { return conf_.conf_hosts; }

  UTIL_FORCEINLINE void set_conf_http_request_timeout(std::chrono::system_clock::duration v) {
    conf_.http_request_timeout = v;
  }
  UTIL_FORCEINLINE void set_conf_http_initialization_timeout(std::chrono::system_clock::duration v) {
    conf_.http_initialization_timeout = v;
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_http_timeout() const {
    if (check_flag(flag_t::READY)) {
      return conf_.http_request_timeout;
    } else {
      return conf_.http_initialization_timeout;
    }
  }
  LIBATAPP_MACRO_API time_t get_http_timeout_ms() const noexcept;

  UTIL_FORCEINLINE void set_conf_http_connect_timeout(std::chrono::system_clock::duration v) {
    conf_.http_connect_timeout = v;
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_http_connect_timeout() const {
    return conf_.http_connect_timeout;
  }
  LIBATAPP_MACRO_API time_t get_http_connect_timeout_ms() const noexcept;

  UTIL_FORCEINLINE void set_conf_dns_cache_timeout(std::chrono::system_clock::duration v) {
    conf_.dns_cache_timeout = v;
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_dns_cache_timeout() const {
    return conf_.dns_cache_timeout;
  }

  UTIL_FORCEINLINE void set_conf_dns_servers(const std::string &servers) { conf_.dns_servers = servers; }
  UTIL_FORCEINLINE const std::string &get_conf_dns_servers() const { return conf_.dns_servers; }

  UTIL_FORCEINLINE void set_conf_etcd_members_update_interval(std::chrono::system_clock::duration v) {
    conf_.etcd_members_update_interval = v;
  }
  UTIL_FORCEINLINE void set_conf_etcd_members_update_interval_min(time_t v) {
    set_conf_etcd_members_update_interval(std::chrono::minutes(v));
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_etcd_members_update_interval() const {
    return conf_.etcd_members_update_interval;
  }

  UTIL_FORCEINLINE void set_conf_etcd_members_retry_interval(std::chrono::system_clock::duration v) {
    conf_.etcd_members_retry_interval = v;
  }
  UTIL_FORCEINLINE void set_conf_etcd_members_retry_interval_min(time_t v) {
    set_conf_etcd_members_retry_interval(std::chrono::minutes(v));
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_etcd_members_retry_interval() const {
    return conf_.etcd_members_retry_interval;
  }

  UTIL_FORCEINLINE void set_conf_keepalive_timeout(std::chrono::system_clock::duration v) {
    conf_.keepalive_timeout = v;
  }
  UTIL_FORCEINLINE void set_conf_keepalive_timeout_sec(time_t v) {
    set_conf_keepalive_timeout(std::chrono::seconds(v));
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_keepalive_timeout() const {
    return conf_.keepalive_timeout;
  }

  UTIL_FORCEINLINE void set_conf_keepalive_interval(std::chrono::system_clock::duration v) {
    conf_.keepalive_interval = v;
  }
  UTIL_FORCEINLINE void set_conf_keepalive_interval_sec(time_t v) {
    set_conf_keepalive_interval(std::chrono::seconds(v));
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_keepalive_interval() const {
    return conf_.keepalive_interval;
  }

  UTIL_FORCEINLINE void set_conf_keepalive_retry_interval(std::chrono::system_clock::duration v) {
    conf_.keepalive_retry_interval = v;
  }
  UTIL_FORCEINLINE void set_conf_keepalive_retry_interval_sec(time_t v) {
    set_conf_keepalive_retry_interval(std::chrono::seconds(v));
  }
  UTIL_FORCEINLINE const std::chrono::system_clock::duration &get_conf_keepalive_retry_interval() const {
    return conf_.keepalive_retry_interval;
  }

  UTIL_FORCEINLINE void set_conf_keepalive_retry_times(size_t v) { conf_.keepalive_retry_times = v; }
  UTIL_FORCEINLINE size_t get_conf_keepalive_retry_times() const { return conf_.keepalive_retry_times; }

  UTIL_FORCEINLINE void set_conf_ssl_enable_alpn(bool v) { conf_.ssl_enable_alpn = v; }
  UTIL_FORCEINLINE bool get_conf_ssl_enable_alpn() const { return conf_.ssl_enable_alpn; }

  UTIL_FORCEINLINE void set_conf_ssl_verify_peer(bool v) { conf_.ssl_verify_peer = v; }
  UTIL_FORCEINLINE bool get_conf_ssl_verify_peer() const { return conf_.ssl_verify_peer; }

  UTIL_FORCEINLINE void set_conf_http_debug_mode(bool v) { conf_.http_debug_mode = v; }
  UTIL_FORCEINLINE bool get_conf_http_debug_mode() const { return conf_.http_debug_mode; }

  UTIL_FORCEINLINE void set_conf_etcd_members_auto_update_hosts(bool v) { conf_.auto_update_hosts = v; }
  UTIL_FORCEINLINE bool get_conf_etcd_members_auto_update_hosts() const { return conf_.auto_update_hosts; }

  UTIL_FORCEINLINE void set_conf_ssl_min_version(ssl_version_t::type v) { conf_.ssl_min_version = v; }
  UTIL_FORCEINLINE ssl_version_t::type get_conf_ssl_min_version() const { return conf_.ssl_min_version; }

  UTIL_FORCEINLINE void set_conf_user_agent(const std::string &v) { conf_.user_agent = v; }
  UTIL_FORCEINLINE const std::string &get_conf_user_agent() const { return conf_.user_agent; }

  UTIL_FORCEINLINE void set_conf_proxy(const std::string &v) { conf_.proxy = v; }
  UTIL_FORCEINLINE const std::string &get_conf_proxy() const { return conf_.proxy; }

  UTIL_FORCEINLINE void set_conf_no_proxy(const std::string &v) { conf_.no_proxy = v; }
  UTIL_FORCEINLINE const std::string &get_conf_no_proxy() const { return conf_.no_proxy; }

  UTIL_FORCEINLINE void set_conf_proxy_user_name(const std::string &v) { conf_.proxy_user_name = v; }
  UTIL_FORCEINLINE const std::string &get_conf_proxy_user_name() const { return conf_.proxy_user_name; }

  UTIL_FORCEINLINE void set_conf_proxy_password(const std::string &v) { conf_.proxy_password = v; }
  UTIL_FORCEINLINE const std::string &get_conf_proxy_password() const { return conf_.proxy_password; }

  UTIL_FORCEINLINE void set_conf_ssl_client_cert(const std::string &v) { conf_.ssl_client_cert = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_cert() const { return conf_.ssl_client_cert; }

  UTIL_FORCEINLINE void set_conf_ssl_client_cert_type(const std::string &v) { conf_.ssl_client_cert_type = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_cert_type() const { return conf_.ssl_client_cert_type; }

  UTIL_FORCEINLINE void set_conf_ssl_client_key(const std::string &v) { conf_.ssl_client_key = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_key() const { return conf_.ssl_client_key; }

  UTIL_FORCEINLINE void set_conf_ssl_client_key_type(const std::string &v) { conf_.ssl_client_key_type = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_key_type() const { return conf_.ssl_client_key_type; }

  UTIL_FORCEINLINE void set_conf_ssl_client_key_passwd(const std::string &v) { conf_.ssl_client_key_passwd = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_key_passwd() const { return conf_.ssl_client_key_passwd; }

  UTIL_FORCEINLINE void set_conf_ssl_ca_cert(const std::string &v) { conf_.ssl_ca_cert = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_ca_cert() const { return conf_.ssl_ca_cert; }

  UTIL_FORCEINLINE void set_conf_ssl_client_tlsauth_username(const std::string &v) {
    conf_.ssl_client_tlsauth_username = v;
  }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_tlsauth_username() const {
    return conf_.ssl_client_tlsauth_username;
  }

  UTIL_FORCEINLINE void set_conf_ssl_client_tlsauth_password(const std::string &v) {
    conf_.ssl_client_tlsauth_password = v;
  }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_client_tlsauth_password() const {
    return conf_.ssl_client_tlsauth_password;
  }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_cert(const std::string &v) { conf_.ssl_proxy_cert = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_cert() const { return conf_.ssl_proxy_cert; }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_cert_type(const std::string &v) { conf_.ssl_proxy_cert_type = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_cert_type() const { return conf_.ssl_proxy_cert_type; }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_key(const std::string &v) { conf_.ssl_proxy_key = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_key() const { return conf_.ssl_proxy_key; }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_key_type(const std::string &v) { conf_.ssl_proxy_key_type = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_key_type() const { return conf_.ssl_proxy_key_type; }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_key_passwd(const std::string &v) { conf_.ssl_proxy_key_passwd = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_key_passwd() const { return conf_.ssl_proxy_key_passwd; }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_tlsauth_username(const std::string &v) {
    conf_.ssl_proxy_tlsauth_username = v;
  }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_tlsauth_username() const {
    return conf_.ssl_proxy_tlsauth_username;
  }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_tlsauth_password(const std::string &v) {
    conf_.ssl_proxy_tlsauth_password = v;
  }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_tlsauth_password() const {
    return conf_.ssl_proxy_tlsauth_password;
  }

  UTIL_FORCEINLINE void set_conf_ssl_proxy_ca_cert(const std::string &v) { conf_.ssl_proxy_ca_cert = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_proxy_ca_cert() const { return conf_.ssl_proxy_ca_cert; }

  UTIL_FORCEINLINE void set_conf_ssl_cipher_list(const std::string &v) { conf_.ssl_cipher_list = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_cipher_list() const { return conf_.ssl_cipher_list; }

  UTIL_FORCEINLINE void set_conf_ssl_cipher_list_tls13(const std::string &v) { conf_.ssl_cipher_list_tls13 = v; }
  UTIL_FORCEINLINE const std::string &get_conf_ssl_cipher_list_tls13() const { return conf_.ssl_cipher_list_tls13; }

  // ================== apis for sub-services ==================
  LIBATAPP_MACRO_API bool add_keepalive(const std::shared_ptr<etcd_keepalive> &keepalive);
  LIBATAPP_MACRO_API bool add_retry_keepalive(const std::shared_ptr<etcd_keepalive> &keepalive);
  LIBATAPP_MACRO_API bool remove_keepalive(std::shared_ptr<etcd_keepalive> keepalive);
  LIBATAPP_MACRO_API bool add_watcher(const std::shared_ptr<etcd_watcher> &watcher);
  LIBATAPP_MACRO_API bool remove_watcher(std::shared_ptr<etcd_watcher> watcher);

  // ================== apis of create request for key-value operation ==================
 public:
  /**
   * @brief               create request for range get key-value data
   * @param key	        key is the first key for the range. If range_end is not given, the request only looks up key.
   * @param range_end	    range_end is the upper bound on the requested range [key, range_end). just like
   * etcd_packer::pack_key_range
   * @param limit	        limit is a limit on the number of keys returned for the request. When limit is set to 0,
   * it is treated as no limit.
   * @param revision	    revision is the point-in-time of the key-value store to use for the range. If revision is
   * less or equal to zero, the range is over the newest key-value store. If the revision has been compacted,
   * ErrCompacted is returned as a response.
   * @return http request
   */
  LIBATAPP_MACRO_API util::network::http_request::ptr_t create_request_kv_get(const std::string &key,
                                                                              const std::string &range_end = "",
                                                                              int64_t limit = 0, int64_t revision = 0);

  /**
   * @brief               create request for set key-value data
   * @param key	        key is the key, in bytes, to put into the key-value store.
   * @param value	        value is the value, in bytes, to associate with the key in the key-value store.
   * @param assign_lease	if add lease ID to associate with the key in the key-value store. A lease value of 0
   * indicates no lease.
   * @param prev_kv	    If prev_kv is set, etcd gets the previous key-value pair before changing it. The previous
   * key-value pair will be returned in the put response.
   * @param ignore_value	If ignore_value is set, etcd updates the key using its current value. Returns an error
   * if the key does not exist.
   * @param ignore_lease	If ignore_lease is set, etcd updates the key using its current lease. Returns an error
   * if the key does not exist.
   * @return http request
   */
  LIBATAPP_MACRO_API util::network::http_request::ptr_t create_request_kv_set(
      const std::string &key, const std::string &value, bool assign_lease = false, bool prev_kv = false,
      bool ignore_value = false, bool ignore_lease = false);

  /**
   * @brief               create request for range delete key-value data
   * @param key	        key is the first key for the range. If range_end is not given, the request only looks up key.
   * @param range_end	    range_end is the upper bound on the requested range [key, range_end). just like
   * etcd_packer::pack_key_range
   * @param prev_kv	    If prev_kv is set, etcd gets the previous key-value pairs before deleting it. The previous
   * key-value pairs will be returned in the delete response.
   * @return http request
   */
  LIBATAPP_MACRO_API util::network::http_request::ptr_t create_request_kv_del(const std::string &key,
                                                                              const std::string &range_end = "",
                                                                              bool prev_kv = false);

  /**
   * @brief                   create request for watch
   * @param key	            key is the first key for the range. If range_end is not given, the request only looks up
   * key.
   * @param range_end	        range_end is the upper bound on the requested range [key, range_end). just like
   * etcd_packer::pack_key_range
   * @param start_revision	start_revision is an optional revision to watch from (inclusive). No start_revision or 0
   * is "now".
   * @param prev_kv	        If prev_kv is set, created watcher gets the previous KV before the event happens. If the
   * previous KV is already compacted, nothing will be returned.
   * @param progress_notify   progress_notify is set so that the etcd server will periodically send a WatchResponse with
   * no events to the new watcher if there are no recent events. It is useful when clients wish to recover a
   * disconnected watcher starting from a recent known revision. The etcd server may decide how often it will send
   * notifications based on current load.
   * @return http request
   */
  LIBATAPP_MACRO_API util::network::http_request::ptr_t create_request_watch(const std::string &key,
                                                                             const std::string &range_end = "",
                                                                             int64_t start_revision = 0,
                                                                             bool prev_kv = false,
                                                                             bool progress_notify = true);

  UTIL_FORCEINLINE int64_t get_lease() const { return conf_.lease; }

  LIBATAPP_MACRO_API on_event_up_down_handle_t add_on_event_up(on_event_up_down_fn_t fn,
                                                               bool trigger_if_running = false);
  LIBATAPP_MACRO_API void remove_on_event_up(on_event_up_down_handle_t &handle);
  LIBATAPP_MACRO_API void reset_on_event_up_handle(on_event_up_down_handle_t &handle);

  LIBATAPP_MACRO_API on_event_up_down_handle_t add_on_event_down(on_event_up_down_fn_t fn,
                                                                 bool trigger_if_not_running = false);
  LIBATAPP_MACRO_API void remove_on_event_down(on_event_up_down_handle_t &handle);
  LIBATAPP_MACRO_API void reset_on_event_down_handle(on_event_up_down_handle_t &handle);

 private:
  void remove_keepalive_path(etcd_keepalive_deletor *keepalive_deletor, bool delay_delete);
  static int libcurl_callback_on_remove_keepalive_path(util::network::http_request &req);

  void retry_pending_actions();
  void set_lease(int64_t v, bool force_active_keepalives);

  bool create_request_auth_authenticate();
  static int libcurl_callback_on_auth_authenticate(util::network::http_request &req);
  bool create_request_auth_user_get();
  static int libcurl_callback_on_auth_user_get(util::network::http_request &req);

  bool retry_request_member_update(const std::string &bad_url);
  bool create_request_member_update();
  static int libcurl_callback_on_member_update(util::network::http_request &req);

  bool create_request_lease_grant();
  bool create_request_lease_keepalive();
  static int libcurl_callback_on_lease_keepalive(util::network::http_request &req);
  util::network::http_request::ptr_t create_request_lease_revoke();

  void add_stats_error_request();
  void add_stats_success_request();
  void add_stats_create_request();

  bool check_authorization() const;

  static void delete_keepalive_deletor(etcd_keepalive_deletor *in, bool close_rpc);
  void cleanup_keepalive_deletors();
  bool select_cluster_member();

 public:
  /**
   * @see https://github.com/grpc-ecosystem/grpc-gateway/blob/master/runtime/errors.go
   * @see https://golang.org/pkg/net/http/
   */
  LIBATAPP_MACRO_API void check_authorization_expired(int http_code, const std::string &content);

  /**
   * @see https://curl.se/libcurl/c/libcurl-errors.html
   */
  LIBATAPP_MACRO_API void check_socket_error_code(int socket_code);

  LIBATAPP_MACRO_API void setup_http_request(util::network::http_request::ptr_t &req, rapidjson::Document &doc,
                                             time_t timeout);

 private:
  using etcd_keepalive_deletor_map_t = std::unordered_map<std::string, etcd_keepalive_deletor *>;

  uint32_t flags_;
  util::random::mt19937 random_generator_;
  conf_t conf_;
  stats_t stats_;
  util::log::log_wrapper::ptr_t logger_;
  util::log::log_formatter::level_t::type startup_log_level_;
  util::log::log_formatter::level_t::type runtime_log_level_;

  util::network::http_request::curl_m_bind_ptr_t curl_multi_;
  util::network::http_request::ptr_t rpc_authenticate_;
  util::network::http_request::ptr_t rpc_update_members_;
  util::network::http_request::ptr_t rpc_keepalive_;
  std::vector<std::shared_ptr<etcd_keepalive> > keepalive_actors_;
  std::vector<std::shared_ptr<etcd_keepalive> > keepalive_retry_actors_;
  etcd_keepalive_deletor_map_t keepalive_deletors_;
  std::vector<std::shared_ptr<etcd_watcher> > watcher_actors_;

  on_event_up_down_handle_set_t event_on_up_callbacks_;
  on_event_up_down_handle_set_t event_on_down_callbacks_;
};

#ifdef _MSC_VER
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(cluster, ...) \
    FWLOGTRACE(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                             \
      FWINSTLOGTRACE(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(cluster, ...) \
    FWLOGDEBUG(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                             \
      FWINSTLOGDEBUG(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_NOTICE(cluster, ...) \
    FWLOGNOTICE(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                              \
      FWINSTLOGNOTICE(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(cluster, ...) \
    FWLOGINFO(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                            \
      FWINSTLOGINFO(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_WARNING(cluster, ...) \
    FWLOGWARNING(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                               \
      FWINSTLOGWARNING(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(cluster, ...) \
    FWLOGERROR(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                             \
      FWINSTLOGERROR(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_FATAL(cluster, ...) \
    FWLOGFATAL(__VA_ARGS__);                                  \
    if ((cluster).get_logger()) {                             \
      FWINSTLOGFATAL(*(cluster).get_logger(), __VA_ARGS__);   \
    }
#else
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(cluster, args...) \
    FWLOGTRACE(args);                                             \
    if ((cluster).get_logger()) {                                 \
      FWINSTLOGTRACE(*(cluster).get_logger(), ##args);            \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(cluster, args...) \
    FWLOGDEBUG(args);                                             \
    if ((cluster).get_logger()) {                                 \
      FWINSTLOGDEBUG(*(cluster).get_logger(), ##args);            \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_NOTICE(cluster, args...) \
    FWLOGNOTICE(args);                                             \
    if ((cluster).get_logger()) {                                  \
      FWINSTLOGNOTICE(*(cluster).get_logger(), ##args);            \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(cluster, args...) \
    FWLOGINFO(args);                                             \
    if ((cluster).get_logger()) {                                \
      FWINSTLOGINFO(*(cluster).get_logger(), ##args);            \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_WARNING(cluster, args...) \
    FWLOGWARNING(args);                                             \
    if ((cluster).get_logger()) {                                   \
      FWINSTLOGWARNING(*(cluster).get_logger(), ##args);            \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(cluster, args...) \
    FWLOGERROR(args);                                             \
    if ((cluster).get_logger()) {                                 \
      FWINSTLOGERROR(*(cluster).get_logger(), ##args);            \
    }
#  define LIBATAPP_MACRO_ETCD_CLUSTER_LOG_FATAL(cluster, args...) \
    FWLOGFATAL(args);                                             \
    if ((cluster).get_logger()) {                                 \
      FWINSTLOGFATAL(*(cluster).get_logger(), ##args);            \
    }
#endif

}  // namespace atapp
