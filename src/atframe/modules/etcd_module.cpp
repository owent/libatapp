// Copyright 2026 atframework
//
// Created by owent

#include "atframe/modules/etcd_module.h"

// clang-format off
#include <config/compiler/protobuf_prefix.h>
// clang-format on

#include <google/protobuf/util/json_util.h>

// clang-format off
#include <config/compiler/protobuf_suffix.h>
// clang-format on

#include <common/string_oprs.h>
#include <random/random_generator.h>

#include <atframe/atapp.h>
#include <atframe/atapp_conf.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(max)
#  undef max
#endif

#if defined(min)
#  undef min
#endif

#define ETCD_MODULE_STARTUP_RETRY_TIMES 5

LIBATAPP_MACRO_NAMESPACE_BEGIN
namespace {
struct ATFW_UTIL_SYMBOL_LOCAL init_timer_data_type {
  etcd_cluster *cluster = nullptr;
  std::chrono::system_clock::time_point timeout;
};

static void init_timer_tick_callback(uv_timer_t *handle) {
  if (nullptr == handle->data) {
    uv_stop(handle->loop);
    return;
  }
  assert(handle);
  assert(handle->loop);

  atfw::util::time::time_utility::update();
  init_timer_data_type *data = reinterpret_cast<init_timer_data_type *>(handle->data);
  data->cluster->tick();

  if (std::chrono::system_clock::now() > data->timeout) {
    uv_stop(handle->loop);
  }
}

static void init_timer_closed_callback(uv_handle_t *handle) {
  if (nullptr == handle->data) {
    uv_stop(handle->loop);
    return;
  }
  assert(handle);
  assert(handle->loop);

  handle->data = nullptr;
  uv_stop(handle->loop);
}

}  // namespace

LIBATAPP_MACRO_API etcd_module::etcd_module() : enable_(false), atapp_(nullptr) {}

LIBATAPP_MACRO_API etcd_module::~etcd_module() { reset(); }

LIBATAPP_MACRO_API int etcd_module::init(atframework::atapp::app &app, const atapp::protocol::atapp_etcd &conf,
                                         const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf) {
  atapp_ = &app;
  load_cluster_conf(conf, log_conf);
  if (is_etcd_enabled()) {
    cluster_.init(atapp_->get_shared_curl_multi_context());
  } else {
    FWLOGINFO("cluster is disabled");
  }
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::reload(const atapp::protocol::atapp_etcd &conf,
                                           const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf) {
  if (atapp_ == nullptr) {
    return 0;
  }
  load_cluster_conf(conf, log_conf);
  return 0;
}

LIBATAPP_MACRO_API void etcd_module::reset() {
  conf_path_cache_.clear();
  conf_cache_.Clear();
  if (cleanup_request_) {
    cleanup_request_->set_priv_data(nullptr);
    cleanup_request_->set_on_complete(nullptr);
    cleanup_request_->stop();
    cleanup_request_.reset();
  }
  cluster_.reset();
}

LIBATAPP_MACRO_API bool etcd_module::check_keepalive_actor_start_success(
    gsl::span<const std::list<etcd_keepalive::ptr_t> *> keepalive_actors) {
  if (atapp_ == nullptr || !is_etcd_enabled()) {
    return false;
  }

  // setup timer for timeout
  uv_timer_t tick_timer;
  init_timer_data_type tick_timer_data;
  uv_timer_init(atapp_->get_bus_node()->get_evloop(), &tick_timer);

  tick_timer_data.cluster = &cluster_;
  tick_timer_data.timeout = std::chrono::system_clock::now();
  tick_timer_data.timeout += std::chrono::seconds(conf_cache_.init().timeout().seconds());
  tick_timer_data.timeout += std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::nanoseconds(conf_cache_.init().timeout().nanos()));
  tick_timer.data = &tick_timer_data;

  uv_timer_start(&tick_timer, init_timer_tick_callback, 128, 128);

  int ticks = 0;
  bool is_failed = false;

  size_t keepalive_count = 0;
  for (const auto *keepalive_actor_list : keepalive_actors) {
    if (keepalive_actor_list != nullptr) {
      keepalive_count += keepalive_actor_list->size();
    }
  }

  while (false == is_failed && std::chrono::system_clock::now() <= tick_timer_data.timeout) {
    atfw::util::time::time_utility::update();
    cluster_.tick();
    ++ticks;

    size_t run_count = 0;
    // Check keepalives
    for (const auto *keepalive_actor_list : keepalive_actors) {
      if (keepalive_actor_list == nullptr) {
        continue;
      }
      for (std::list<etcd_keepalive::ptr_t>::const_iterator iter = keepalive_actor_list->begin();
           iter != keepalive_actor_list->end(); ++iter) {
        if ((*iter)->is_check_run()) {
          if (!(*iter)->is_check_passed()) {
            LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(cluster_, "etcd_keepalive lock {} failed.", (*iter)->get_path());
            is_failed = true;
          }

          ++run_count;
        }
      }
    }

    // Check watchers

    // 全部成功或任意失败则退出
    if (is_failed || run_count >= keepalive_count) {
      break;
    }

    uv_run(atapp_->get_bus_node()->get_evloop(), UV_RUN_ONCE);

    // 任意重试次数过多则失败退出
    for (const auto *keepalive_actor_list : keepalive_actors) {
      if (keepalive_actor_list == nullptr) {
        continue;
      }
      for (std::list<etcd_keepalive::ptr_t>::const_iterator iter = keepalive_actor_list->begin();
           iter != keepalive_actor_list->end(); ++iter) {
        if ((*iter)->get_check_times() >= ETCD_MODULE_STARTUP_RETRY_TIMES ||
            cluster_.get_stats().continue_error_requests > ETCD_MODULE_STARTUP_RETRY_TIMES) {
          size_t retry_times = (*iter)->get_check_times();
          if (cluster_.get_stats().continue_error_requests > retry_times) {
            retry_times = cluster_.get_stats().continue_error_requests;
          }
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(cluster_,
                                                "etcd_keepalive request {} for {} times (with {} ticks) failed.",
                                                (*iter)->get_path(), retry_times, ticks);
          is_failed = true;
          break;
        }
      }
    }

    if (is_failed) {
      break;
    }
  }

  if (std::chrono::system_clock::now() > tick_timer_data.timeout) {
    is_failed = true;
    for (const auto *keepalive_actor_list : keepalive_actors) {
      if (keepalive_actor_list == nullptr) {
        continue;
      }
      for (std::list<etcd_keepalive::ptr_t>::const_iterator iter = keepalive_actor_list->begin();
           iter != keepalive_actor_list->end(); ++iter) {
        size_t retry_times = (*iter)->get_check_times();
        if (cluster_.get_stats().continue_error_requests > retry_times) {
          retry_times = cluster_.get_stats().continue_error_requests;
        }
        if ((*iter)->is_check_passed()) {
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_WARNING(
              cluster_,
              "etcd_keepalive request {} timeout, retry {} times (with {} ticks), check passed, has data: {}.",
              (*iter)->get_path(), retry_times, ticks, (*iter)->has_data() ? "true" : "false");
        } else {
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
              cluster_,
              "etcd_keepalive request {} timeout, retry {} times (with {} ticks), check unpassed, has data: {}.",
              (*iter)->get_path(), retry_times, ticks, (*iter)->has_data() ? "true" : "false");
        }
      }
    }
  }

  // close timer for timeout
  uv_timer_stop(&tick_timer);
  uv_close(reinterpret_cast<uv_handle_t *>(&tick_timer), init_timer_closed_callback);
  while (tick_timer.data != nullptr) {
    uv_run(atapp_->get_bus_node()->get_evloop(), UV_RUN_ONCE);
  }

  return !is_failed;
}

LIBATAPP_MACRO_API int etcd_module::tick() {
  if (atapp_ == nullptr) {
    return 0;
  }

  if (!is_etcd_enabled()) {
    return 0;
  }

  // previous closing not finished, wait for it
  if (cleanup_request_ || atapp_->is_closing()) {
    return cluster_.tick();
  }

  // first startup when reloaded
  if (cluster_.check_flag(etcd_cluster::flag_t::kClosing)) {
    cluster_.init(atapp_->get_shared_curl_multi_context());
  }

  return cluster_.tick();
}

LIBATAPP_MACRO_API int etcd_module::stop() {
  if (atapp_ == nullptr) {
    return 0;
  }

  if (!cleanup_request_ && !cluster_.check_flag(etcd_cluster::flag_t::kClosing)) {
    bool revoke_lease = true;
    if (atapp_->is_current_upgrade_mode()) {
      revoke_lease = false;
    }

    cleanup_request_ = cluster_.close(false, revoke_lease);

    if (cleanup_request_ && cleanup_request_->is_running()) {
      cleanup_request_->set_priv_data(this);
      cleanup_request_->set_on_complete(http_callback_on_etcd_closed);
    }
  }

  if (cleanup_request_ && cleanup_request_->is_running()) {
    return 1;
  }

  // recycle all resources
  reset();
  return 0;
}

void etcd_module::load_cluster_conf(const atapp::protocol::atapp_etcd &conf,
                                    const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf) {
  // 不支持动态关闭 etcd 功能
  if (is_etcd_enabled() && (!conf.enable() || conf.hosts_size() == 0)) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_WARNING(
        cluster_,
        "etcd cluster is disabled by new configure, but it's enabled by old configure, dynamic disable is not "
        "supported, please restart app to apply the new configure.");
    return;
  }

  // load configure
  conf_cache_ = conf;
  enable_ = conf.enable();

  // load logger
  atfw::util::log::log_wrapper::ptr_t logger;
  atfw::util::log::log_level startup_level = atfw::util::log::log_level::kDisabled;
  do {
    atapp::protocol::atapp_log etcd_log_conf;
    if (log_conf != nullptr) {
      etcd_log_conf = *log_conf;
    } else {
      atapp_->parse_log_configures_into(etcd_log_conf, std::vector<gsl::string_view>{"atapp", "etcd", "log"},
                                        "ATAPP_ETCD_LOG");
    }
    if (etcd_log_conf.category_size() <= 0) {
      break;
    }

    startup_level = atfw::util::log::log_formatter::get_level_by_name(conf.log().startup_level());
    if (startup_level >= atfw::util::log::log_level::kDisabled &&
        atfw::util::log::log_formatter::get_level_by_name(etcd_log_conf.level()) >=
            atfw::util::log::log_level::kDisabled) {
      break;
    }

    logger = atfw::util::log::log_wrapper::create_user_logger();
    if (!logger) {
      break;
    }
    logger->init(atfw::util::log::log_formatter::get_level_by_name(etcd_log_conf.level()));
    atapp_->setup_logger(*logger, etcd_log_conf.level(), etcd_log_conf.category(0));
  } while (false);
  cluster_.set_logger(logger, startup_level);

  // etcd context configure
  {
    std::vector<std::string> conf_hosts;
    conf_hosts.reserve(static_cast<size_t>(conf.hosts_size()));
    for (int i = 0; i < conf.hosts_size(); ++i) {
      conf_hosts.push_back(conf.hosts(i));
    }
    if (!conf_hosts.empty()) {
      cluster_.set_conf_hosts(conf_hosts);
    }
  }

  cluster_.set_conf_authorization(conf.authorization());
  cluster_.set_conf_http_request_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().timeout(), std::chrono::milliseconds(10000)));
  cluster_.set_conf_http_initialization_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().initialization_timeout(), std::chrono::milliseconds(3000)));
  cluster_.set_conf_http_connect_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().connect_timeout(), std::chrono::milliseconds(0)));
  cluster_.set_conf_dns_cache_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().dns_cache_timeout(), std::chrono::milliseconds(0)));
  cluster_.set_conf_dns_servers(conf.request().dns_servers());
  cluster_.set_conf_etcd_members_auto_update_hosts(conf.cluster().auto_update());
  cluster_.set_conf_etcd_members_update_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.cluster().update_interval(), std::chrono::milliseconds(300000)));
  cluster_.set_conf_etcd_members_retry_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.cluster().retry_interval(), std::chrono::milliseconds(60000)));

  cluster_.set_conf_keepalive_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.keepalive().timeout(), std::chrono::milliseconds(16000)));
  cluster_.set_conf_keepalive_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.keepalive().ttl(), std::chrono::milliseconds(5000)));
  cluster_.set_conf_keepalive_retry_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.keepalive().retry_interval(), std::chrono::milliseconds(3000)));

  // HTTP
  if (!conf.http().user_agent().empty()) {
    cluster_.set_conf_user_agent(conf.http().user_agent());
  }
  if (!conf.http().proxy().empty()) {
    cluster_.set_conf_proxy(conf.http().proxy());
  }
  if (!conf.http().no_proxy().empty()) {
    cluster_.set_conf_no_proxy(conf.http().no_proxy());
  }
  if (!conf.http().proxy_user_name().empty()) {
    cluster_.set_conf_proxy_user_name(conf.http().proxy_user_name());
  }
  if (!conf.http().proxy_password().empty()) {
    cluster_.set_conf_proxy_password(conf.http().proxy_password());
  }

  cluster_.set_conf_http_debug_mode(conf.http().debug());

  // SSL configure
  cluster_.set_conf_ssl_enable_alpn(conf.ssl().enable_alpn());
  cluster_.set_conf_ssl_verify_peer(conf.ssl().verify_peer());
  do {
    const std::string &ssl_version = conf.ssl().ssl_min_version();
    if (ssl_version.empty()) {
      break;
    }
    if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.3", 7) ||
        0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv13", 6)) {
      cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV13);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.2", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv12", 6)) {
      cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV12);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.1", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv11", 6)) {
      cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV11);
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP(ssl_version.c_str(), "TLSv1")) {
      cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV11);
    } else {
      cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kDisabled);
    }
  } while (false);

  if (!conf.ssl().ssl_client_cert().empty()) {
    cluster_.set_conf_ssl_client_cert(conf.ssl().ssl_client_cert());
  }

  if (!conf.ssl().ssl_client_cert_type().empty()) {
    cluster_.set_conf_ssl_client_cert_type(conf.ssl().ssl_client_cert_type());
  }

  if (!conf.ssl().ssl_client_key().empty()) {
    cluster_.set_conf_ssl_client_key(conf.ssl().ssl_client_key());
  }

  if (!conf.ssl().ssl_client_key_type().empty()) {
    cluster_.set_conf_ssl_client_key_type(conf.ssl().ssl_client_key_type());
  }

  if (!conf.ssl().ssl_client_key_passwd().empty()) {
    cluster_.set_conf_ssl_client_key_passwd(conf.ssl().ssl_client_key_passwd());
  }

  if (!conf.ssl().ssl_ca_cert().empty()) {
    cluster_.set_conf_ssl_ca_cert(conf.ssl().ssl_ca_cert());
  }

  if (!conf.ssl().ssl_proxy_cert().empty()) {
    cluster_.set_conf_ssl_proxy_cert(conf.ssl().ssl_proxy_cert());
  }

  if (!conf.ssl().ssl_proxy_cert_type().empty()) {
    cluster_.set_conf_ssl_proxy_cert_type(conf.ssl().ssl_proxy_cert_type());
  }

  if (!conf.ssl().ssl_proxy_key().empty()) {
    cluster_.set_conf_ssl_proxy_key(conf.ssl().ssl_proxy_key());
  }

  if (!conf.ssl().ssl_proxy_key_type().empty()) {
    cluster_.set_conf_ssl_proxy_key_type(conf.ssl().ssl_proxy_key_type());
  }

  if (!conf.ssl().ssl_proxy_key_passwd().empty()) {
    cluster_.set_conf_ssl_proxy_key_passwd(conf.ssl().ssl_proxy_key_passwd());
  }

  if (!conf.ssl().ssl_proxy_ca_cert().empty()) {
    cluster_.set_conf_ssl_proxy_ca_cert(conf.ssl().ssl_proxy_ca_cert());
  }

  if (!conf.ssl().ssl_cipher_list().empty()) {
    cluster_.set_conf_ssl_cipher_list(conf.ssl().ssl_cipher_list());
  }

  if (!conf.ssl().ssl_cipher_list_tls13().empty()) {
    cluster_.set_conf_ssl_cipher_list_tls13(conf.ssl().ssl_cipher_list_tls13());
  }
}

LIBATAPP_MACRO_API std::string etcd_module::generate_etcd_path(const std::string &path) {
  std::string gen_path = path;
  if (!gen_path.empty()) {
    size_t last_idx = gen_path.size() - 1;
    if (gen_path[last_idx] != '/' && gen_path[last_idx] != '\\') {
      gen_path += '/';
    }
  } else {
    gen_path = std::string("/");
  }
  return gen_path;
}

int etcd_module::http_callback_on_etcd_closed(atfw::util::network::http_request &req) {
  etcd_module *self = reinterpret_cast<etcd_module *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("etcd_module get request shouldn't has request without private data");
    return 0;
  }

  self->reset();
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(self->cluster_, "Etcd revoke lease finished");

  if (self->atapp_ != nullptr) {
    self->atapp_->stop();
  }
  return 0;
}

LIBATAPP_MACRO_API bool etcd_module::is_etcd_enabled() const { return !cluster_.get_conf_hosts().empty() && enable_; }

LIBATAPP_MACRO_API atapp::etcd_cluster &etcd_module::get_etcd_cluster() { return cluster_; }

LIBATAPP_MACRO_API const atapp::etcd_cluster &etcd_module::get_etcd_cluster() const { return cluster_; }

LIBATAPP_MACRO_API const std::string &etcd_module::get_configure_path() {
  if (conf_path_cache_.empty()) {
    conf_path_cache_ = generate_etcd_path(conf_cache_.path());
  }
  return conf_path_cache_;
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &etcd_module::get_configure() const { return conf_cache_; }

LIBATAPP_MACRO_NAMESPACE_END
