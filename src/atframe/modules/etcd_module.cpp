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

LIBATAPP_MACRO_API etcd_module::etcd_module() : etcd_enabled_(false) {}

LIBATAPP_MACRO_API etcd_module::~etcd_module() { reset(); }

LIBATAPP_MACRO_API void etcd_module::reset() {
  custom_data_.clear();
  for (auto &cluster_holder : clusters_) {
    cluster_holder->reset();
  }

  clusters_.clear();

  if (curl_multi_) {
    atfw::util::network::http_request::destroy_curl_multi(curl_multi_);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LIBATAPP_MACRO_API int etcd_module::init() {
  // init curl
  int res = curl_global_init(CURL_GLOBAL_ALL);
  if (res) {
    FWLOGERROR("init cURL failed, errcode: {}", res);
    return -1;
  }

  atfw::util::network::http_request::curl_share_options share_options;
  atfw::util::network::http_request::curl_multi_options multi_options;
  multi_options.ev_loop = get_app()->get_bus_node()->get_evloop();
  // Share DNS cache, connection, TLS sessions and etc.
  atfw::util::network::http_request::create_curl_share(share_options, multi_options.share_context);
  atfw::util::network::http_request::create_curl_multi(get_app()->get_bus_node()->get_evloop(), curl_multi_);
  if (!curl_multi_) {
    FWLOGERROR("create curl multi instance failed.");
    return -1;
  }

  if (!etcd_enabled_) {
    FWLOGINFO("etcd disabled, start single mode");
  }
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::init_cluster(
    etcd_cluster_holder_ptr_t cluster_holder, etcd_cluster_load_conf_func_t load_conf_func,
    etcd_cluster_init_keepalive_watcher_func_t init_keepalive_watcher_func,
    etcd_cluster_reset_keepalive_watcher_func_t reset_keepalive_watcher_func) {
  if (load_conf_func == nullptr) {
    FWLOGERROR("load_conf_func is null, can not create cluster.");
    return -1;
  }
  if (cluster_holder == nullptr) {
    FWLOGERROR("cluster_holder is null, can not create cluster.");
    return -1;
  }

  cluster_holder->atapp_ = get_app();
  cluster_holder->module_ = this;
  cluster_holder->load_conf_func_ = std::move(load_conf_func);
  cluster_holder->init_keepalive_watcher_func_ = std::move(init_keepalive_watcher_func);
  cluster_holder->reset_keepalive_watcher_func_ = std::move(reset_keepalive_watcher_func);
  cluster_holder->load_conf_func_(*cluster_holder->atapp_, *cluster_holder);

  if (etcd_enabled_ == false) {
    FWLOGINFO("etcd is disabled, skip init cluster {}", cluster_holder->conf_path_cache_);
    clusters_.push_back(cluster_holder);
    return 0;
  }
  if (cluster_holder->enable_ == false || cluster_holder->conf_path_cache_.empty()) {
    FWLOGINFO("cluster {} is disabled, skip init it", cluster_holder->conf_path_cache_);
    clusters_.push_back(cluster_holder);
    return 0;
  }
  cluster_holder->cluster_.init(curl_multi_);

  if (cluster_holder->init_keepalive_watcher_func_) {
    int result = cluster_holder->init_keepalive_watcher_func_(*cluster_holder->atapp_, *cluster_holder, true);
    if (result != 0) {
      clusters_.push_back(cluster_holder);
      cluster_holder->stop();
      return -1;
    }
  }

  clusters_.push_back(cluster_holder);
  return 0;
}

LIBATAPP_MACRO_API bool etcd_module::check_keepalive_actor_start_success(
    atframework::atapp::app &app, etcd_cluster_holder &cluster_holder,
    gsl::span<const std::list<etcd_keepalive::ptr_t> *> keepalive_actors) {
  // setup timer for timeout
  uv_timer_t tick_timer;
  init_timer_data_type tick_timer_data;
  uv_timer_init(app.get_bus_node()->get_evloop(), &tick_timer);
  auto etcd_module = app.get_etcd_module();
  if (etcd_module == nullptr) {
    FWLOGERROR("etcd_module is null, can not check keepalive actors.");
    return false;
  }

  tick_timer_data.cluster = &cluster_holder.cluster_;
  tick_timer_data.timeout = std::chrono::system_clock::now();
  tick_timer_data.timeout += std::chrono::seconds(cluster_holder.conf_cache_.init().timeout().seconds());
  tick_timer_data.timeout += std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::nanoseconds(cluster_holder.conf_cache_.init().timeout().nanos()));
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
    cluster_holder.cluster_.tick();
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
            LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(cluster_holder.cluster_, "etcd_keepalive lock {} failed.",
                                                  (*iter)->get_path());
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

    uv_run(app.get_bus_node()->get_evloop(), UV_RUN_ONCE);

    // 任意重试次数过多则失败退出
    for (const auto *keepalive_actor_list : keepalive_actors) {
      if (keepalive_actor_list == nullptr) {
        continue;
      }
      for (std::list<etcd_keepalive::ptr_t>::const_iterator iter = keepalive_actor_list->begin();
           iter != keepalive_actor_list->end(); ++iter) {
        if ((*iter)->get_check_times() >= ETCD_MODULE_STARTUP_RETRY_TIMES ||
            cluster_holder.cluster_.get_stats().continue_error_requests > ETCD_MODULE_STARTUP_RETRY_TIMES) {
          size_t retry_times = (*iter)->get_check_times();
          if (cluster_holder.cluster_.get_stats().continue_error_requests > retry_times) {
            retry_times = cluster_holder.cluster_.get_stats().continue_error_requests;
          }
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(cluster_holder.cluster_,
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
        if (cluster_holder.cluster_.get_stats().continue_error_requests > retry_times) {
          retry_times = cluster_holder.cluster_.get_stats().continue_error_requests;
        }
        if ((*iter)->is_check_passed()) {
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_WARNING(
              cluster_holder.cluster_,
              "etcd_keepalive request {} timeout, retry {} times (with {} ticks), check passed, has data: {}.",
              (*iter)->get_path(), retry_times, ticks, (*iter)->has_data() ? "true" : "false");
        } else {
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
              cluster_holder.cluster_,
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
    uv_run(app.get_bus_node()->get_evloop(), UV_RUN_ONCE);
  }

  return !is_failed;
}

LIBATAPP_MACRO_API int etcd_module::reload() {
  const atapp::protocol::atapp_etcd_common &common_conf = get_app()->get_origin_configure().etcd();
  etcd_enabled_ = common_conf.enable();

  do {
    // load logger
    atapp::protocol::atapp_log etcd_log_conf;
    get_app()->parse_log_configures_into(etcd_log_conf, std::vector<gsl::string_view>{"atapp", "etcd", "log"},
                                         "ATAPP_ETCD_LOG");
    if (etcd_log_conf.category_size() <= 0) {
      break;
    }

    if (atfw::util::log::log_formatter::get_level_by_name(etcd_log_conf.level()) >=
        atfw::util::log::log_level::kDisabled) {
      break;
    }

    logger_ = atfw::util::log::log_wrapper::create_user_logger();
    if (!logger_) {
      break;
    }
    logger_->init(atfw::util::log::log_formatter::get_level_by_name(etcd_log_conf.level()));
    get_app()->setup_logger(*logger_, etcd_log_conf.level(), etcd_log_conf.category(0));
  } while (false);

  for (auto &cluster_holder : clusters_) {
    cluster_holder->load_conf_func_(*cluster_holder->atapp_, *cluster_holder);
  }
  return 0;
}

LIBATAPP_MACRO_API void etcd_module::load_cluster_conf(
    etcd_cluster_holder &cluster_holder, const atapp::protocol::atapp_etcd &conf,
    const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf) {
  cluster_holder.conf_cache_ = conf;
  // load logger
  atfw::util::log::log_wrapper::ptr_t logger;
  atfw::util::log::log_level startup_level = atfw::util::log::log_level::kDisabled;
  do {
    startup_level = atfw::util::log::log_formatter::get_level_by_name(conf.log().startup_level());
    if (startup_level >= atfw::util::log::log_level::kDisabled) {
      break;
    }

    if (log_conf == nullptr) {
      logger = logger_;
      break;
    }

    if (log_conf->category_size() == 0) {
      break;
    }

    logger = atfw::util::log::log_wrapper::create_user_logger();
    if (!logger) {
      break;
    }
    logger->init(atfw::util::log::log_formatter::get_level_by_name(log_conf->level()));
    get_app()->setup_logger(*logger, log_conf->level(), log_conf->category(0));
  } while (false);
  cluster_holder.cluster_.set_logger(logger, startup_level);

  // etcd context configure
  {
    std::vector<std::string> conf_hosts;
    conf_hosts.reserve(static_cast<size_t>(conf.hosts_size()));
    for (int i = 0; i < conf.hosts_size(); ++i) {
      conf_hosts.push_back(conf.hosts(i));
    }
    if (!conf_hosts.empty()) {
      cluster_holder.cluster_.set_conf_hosts(conf_hosts);
    }
  }

  cluster_holder.cluster_.set_conf_authorization(conf.authorization());
  cluster_holder.cluster_.set_conf_http_request_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().timeout(), std::chrono::milliseconds(10000)));
  cluster_holder.cluster_.set_conf_http_initialization_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().initialization_timeout(), std::chrono::milliseconds(3000)));
  cluster_holder.cluster_.set_conf_http_connect_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().connect_timeout(), std::chrono::milliseconds(0)));
  cluster_holder.cluster_.set_conf_dns_cache_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.request().dns_cache_timeout(), std::chrono::milliseconds(0)));
  cluster_holder.cluster_.set_conf_dns_servers(conf.request().dns_servers());
  cluster_holder.cluster_.set_conf_etcd_members_auto_update_hosts(conf.cluster().auto_update());
  cluster_holder.cluster_.set_conf_etcd_members_update_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.cluster().update_interval(), std::chrono::milliseconds(300000)));
  cluster_holder.cluster_.set_conf_etcd_members_retry_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.cluster().retry_interval(), std::chrono::milliseconds(60000)));

  cluster_holder.cluster_.set_conf_keepalive_timeout(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.keepalive().timeout(), std::chrono::milliseconds(16000)));
  cluster_holder.cluster_.set_conf_keepalive_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.keepalive().ttl(), std::chrono::milliseconds(5000)));
  cluster_holder.cluster_.set_conf_keepalive_retry_interval(
      protobuf_to_chrono_convert_duration_with_default<std::chrono::system_clock::duration>(
          conf.keepalive().retry_interval(), std::chrono::milliseconds(3000)));

  // HTTP
  if (!conf.http().user_agent().empty()) {
    cluster_holder.cluster_.set_conf_user_agent(conf.http().user_agent());
  }
  if (!conf.http().proxy().empty()) {
    cluster_holder.cluster_.set_conf_proxy(conf.http().proxy());
  }
  if (!conf.http().no_proxy().empty()) {
    cluster_holder.cluster_.set_conf_no_proxy(conf.http().no_proxy());
  }
  if (!conf.http().proxy_user_name().empty()) {
    cluster_holder.cluster_.set_conf_proxy_user_name(conf.http().proxy_user_name());
  }
  if (!conf.http().proxy_password().empty()) {
    cluster_holder.cluster_.set_conf_proxy_password(conf.http().proxy_password());
  }

  cluster_holder.cluster_.set_conf_http_debug_mode(conf.http().debug());

  // SSL configure
  cluster_holder.cluster_.set_conf_ssl_enable_alpn(conf.ssl().enable_alpn());
  cluster_holder.cluster_.set_conf_ssl_verify_peer(conf.ssl().verify_peer());
  do {
    const std::string &ssl_version = conf.ssl().ssl_min_version();
    if (ssl_version.empty()) {
      break;
    }
    if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.3", 7) ||
        0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv13", 6)) {
      cluster_holder.cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV13);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.2", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv12", 6)) {
      cluster_holder.cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV12);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.1", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv11", 6)) {
      cluster_holder.cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV11);
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP(ssl_version.c_str(), "TLSv1")) {
      cluster_holder.cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kTlsV11);
    } else {
      cluster_holder.cluster_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::kDisabled);
    }
  } while (false);

  if (!conf.ssl().ssl_client_cert().empty()) {
    cluster_holder.cluster_.set_conf_ssl_client_cert(conf.ssl().ssl_client_cert());
  }

  if (!conf.ssl().ssl_client_cert_type().empty()) {
    cluster_holder.cluster_.set_conf_ssl_client_cert_type(conf.ssl().ssl_client_cert_type());
  }

  if (!conf.ssl().ssl_client_key().empty()) {
    cluster_holder.cluster_.set_conf_ssl_client_key(conf.ssl().ssl_client_key());
  }

  if (!conf.ssl().ssl_client_key_type().empty()) {
    cluster_holder.cluster_.set_conf_ssl_client_key_type(conf.ssl().ssl_client_key_type());
  }

  if (!conf.ssl().ssl_client_key_passwd().empty()) {
    cluster_holder.cluster_.set_conf_ssl_client_key_passwd(conf.ssl().ssl_client_key_passwd());
  }

  if (!conf.ssl().ssl_ca_cert().empty()) {
    cluster_holder.cluster_.set_conf_ssl_ca_cert(conf.ssl().ssl_ca_cert());
  }

  if (!conf.ssl().ssl_proxy_cert().empty()) {
    cluster_holder.cluster_.set_conf_ssl_proxy_cert(conf.ssl().ssl_proxy_cert());
  }

  if (!conf.ssl().ssl_proxy_cert_type().empty()) {
    cluster_holder.cluster_.set_conf_ssl_proxy_cert_type(conf.ssl().ssl_proxy_cert_type());
  }

  if (!conf.ssl().ssl_proxy_key().empty()) {
    cluster_holder.cluster_.set_conf_ssl_proxy_key(conf.ssl().ssl_proxy_key());
  }

  if (!conf.ssl().ssl_proxy_key_type().empty()) {
    cluster_holder.cluster_.set_conf_ssl_proxy_key_type(conf.ssl().ssl_proxy_key_type());
  }

  if (!conf.ssl().ssl_proxy_key_passwd().empty()) {
    cluster_holder.cluster_.set_conf_ssl_proxy_key_passwd(conf.ssl().ssl_proxy_key_passwd());
  }

  if (!conf.ssl().ssl_proxy_ca_cert().empty()) {
    cluster_holder.cluster_.set_conf_ssl_proxy_ca_cert(conf.ssl().ssl_proxy_ca_cert());
  }

  if (!conf.ssl().ssl_cipher_list().empty()) {
    cluster_holder.cluster_.set_conf_ssl_cipher_list(conf.ssl().ssl_cipher_list());
  }

  if (!conf.ssl().ssl_cipher_list_tls13().empty()) {
    cluster_holder.cluster_.set_conf_ssl_cipher_list_tls13(conf.ssl().ssl_cipher_list_tls13());
  }

  cluster_holder.enable_ = conf.enable();
  cluster_holder.tick_interval_ = std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::seconds(conf.init().tick_interval().seconds()) +
      std::chrono::nanoseconds(conf.init().tick_interval().nanos()));
  if (cluster_holder.tick_interval_ <
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(32))) {
    cluster_holder.tick_interval_ =
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(128));
  }
  cluster_holder.conf_path_cache_ = generate_etcd_path(conf.path());
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

LIBATAPP_MACRO_API int etcd_module::stop() {
  bool not_stopped_yet = false;
  for (auto &cluster_holder : clusters_) {
    cluster_holder->stop();
    if (!cluster_holder->is_stopped()) {
      not_stopped_yet = true;
    }
  }

  if (not_stopped_yet) {
    return 1;
  }

  // recycle all resources
  reset();
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::timeout() {
  reset();
  return 0;
}

LIBATAPP_MACRO_API const char *ATFW_UTIL_MACRO_NONNULL etcd_module::name() const { return "atapp: etcd module"; }

LIBATAPP_MACRO_API int etcd_module::tick() {
  atframework::atapp::app *owner = get_app();
  if (nullptr == owner) {
    return 0;
  }

  if (etcd_enabled_) {
    // first startup when reloaded
    if (!curl_multi_) {
      int res = init();
      if (res < 0) {
        FWLOGERROR("initialize etcd failed, res: {}", res);
        owner->stop();
        return res;
      }
    }
  }

  for (auto &cluster_holder : clusters_) {
    cluster_holder->tick();
  }
  return 0;
}

LIBATAPP_MACRO_API const std::string &etcd_module::get_conf_custom_data() const { return custom_data_; }
LIBATAPP_MACRO_API void etcd_module::set_conf_custom_data(const std::string &v) { custom_data_ = v; }

LIBATAPP_MACRO_API bool etcd_module::is_etcd_enabled() const { return etcd_enabled_; }
LIBATAPP_MACRO_API void etcd_module::enable_etcd() { etcd_enabled_ = true; }
LIBATAPP_MACRO_API void etcd_module::disable_etcd() { etcd_enabled_ = false; }

LIBATAPP_MACRO_API const atfw::util::network::http_request::curl_m_bind_ptr_t &
etcd_module::get_shared_curl_multi_context() const {
  return curl_multi_;
}

LIBATAPP_MACRO_API etcd_cluster_holder::etcd_cluster_holder()
    : enable_(false),
      atapp_(nullptr),
      module_(nullptr),
      load_conf_func_(nullptr),
      init_keepalive_watcher_func_(nullptr),
      reset_keepalive_watcher_func_(nullptr),
      tick_interval_(std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(128))) {
  tick_next_timepoint_ = app::get_sys_now();
}

LIBATAPP_MACRO_API atapp::etcd_cluster &etcd_cluster_holder::get_etcd_cluster() { return cluster_; }

LIBATAPP_MACRO_API const atapp::etcd_cluster &etcd_cluster_holder::get_etcd_cluster() const { return cluster_; }

LIBATAPP_MACRO_API const std::string &etcd_cluster_holder::get_configure_path() const { return conf_path_cache_; }

LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &etcd_cluster_holder::get_configure() const { return conf_cache_; }

int etcd_cluster_holder::tick() {
  if (atapp_ == nullptr || module_ == nullptr) {
    return 0;
  }
  // Slow down the tick interval of etcd module, it require http request which is very slow compared to atbus
  if (tick_next_timepoint_ >= atapp_->get_last_tick_time()) {
    return 0;
  }
  tick_next_timepoint_ = atapp_->get_last_tick_time() + tick_interval_;

  // single mode
  if (cluster_.get_conf_hosts().empty() || !module_->is_etcd_enabled() || !enable_) {
    // If it's initializing, is_available() will return false
    if (!cleanup_request_ && cluster_.is_available()) {
      bool revoke_lease = true;
      if (atapp_->is_current_upgrade_mode()) {
        revoke_lease = false;
      }

      cleanup_request_ = cluster_.close(false, revoke_lease);
      if (reset_keepalive_watcher_func_) {
        reset_keepalive_watcher_func_(*atapp_, *this);
      }
    }

    return 0;
  }

  // previous closing not finished, wait for it
  if (cleanup_request_ || atapp_->is_closing()) {
    return cluster_.tick();
  }

  if (module_->get_shared_curl_multi_context() &&
      cluster_.check_flag(etcd_cluster::flag_t::kClosing)) {  // Already stoped and restart etcd_ctx_
    cluster_.init(module_->get_shared_curl_multi_context());
    if (init_keepalive_watcher_func_) {
      int result = init_keepalive_watcher_func_(*atapp_, *this, false);
      if (result != 0) {
        return -1;
      }
    }
  }

  return cluster_.tick();
}

void etcd_cluster_holder::stop() {
  if (!cleanup_request_ && !cluster_.check_flag(etcd_cluster::flag_t::kClosing)) {
    bool revoke_lease = true;
    if (atapp_ != nullptr && atapp_->is_current_upgrade_mode()) {
      revoke_lease = false;
    }

    cleanup_request_ = cluster_.close(false, revoke_lease);

    if (cleanup_request_ && cleanup_request_->is_running()) {
      cleanup_request_->set_priv_data(this);
      cleanup_request_->set_on_complete(http_callback_on_etcd_closed);
    }

    if (reset_keepalive_watcher_func_) {
      reset_keepalive_watcher_func_(*atapp_, *this);
    }
  }
}

bool etcd_cluster_holder::is_stopped() const { return !cleanup_request_ || !cleanup_request_->is_running(); }

void etcd_cluster_holder::reset() {
  conf_path_cache_.clear();
  load_conf_func_ = nullptr;
  init_keepalive_watcher_func_ = nullptr;
  reset_keepalive_watcher_func_ = nullptr;
  if (cleanup_request_) {
    cleanup_request_->set_priv_data(nullptr);
    cleanup_request_->set_on_complete(nullptr);
    cleanup_request_->stop();
    cleanup_request_.reset();
  }
}

int etcd_cluster_holder::http_callback_on_etcd_closed(atfw::util::network::http_request &req) {
  etcd_cluster_holder *self = reinterpret_cast<etcd_cluster_holder *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("etcd_module get request shouldn't has request without private data");
    return 0;
  }

  self->reset();

  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(self->cluster_, "Etcd revoke lease finished");
  return 0;
}

LIBATAPP_MACRO_NAMESPACE_END
