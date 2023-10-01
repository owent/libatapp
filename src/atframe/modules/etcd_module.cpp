// Copyright 2021 atframework
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

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define ETCD_MODULE_STARTUP_RETRY_TIMES 5
#define ETCD_MODULE_BY_ID_DIR "by_id"
#define ETCD_MODULE_BY_NAME_DIR "by_name"
#define ETCD_MODULE_BY_TYPE_ID_DIR "by_type_id"
#define ETCD_MODULE_BY_TYPE_NAME_DIR "by_type_name"
#define ETCD_MODULE_BY_TAG_DIR "by_tag"

namespace atapp {
namespace detail {
std::chrono::system_clock::duration convert_to_chrono(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &in,
                                                      time_t default_value_ms) {
  if (in.seconds() <= 0 && in.nanos() <= 0) {
    return std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(default_value_ms));
  }
  return std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(in.seconds())) +
         std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(in.nanos()));
}

template <class T>
static T convert_to_ms(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &in, T default_value) {
  T ret = static_cast<T>(in.seconds() * 1000 + in.nanos() / 1000000);
  if (ret <= 0) {
    ret = default_value;
  }

  return ret;
}

struct init_timer_data_type {
  etcd_cluster *cluster;
  std::chrono::system_clock::time_point timeout;
};

static void init_timer_tick_callback(uv_timer_t *handle) {
  if (nullptr == handle->data) {
    uv_stop(handle->loop);
    return;
  }
  assert(handle);
  assert(handle->loop);

  util::time::time_utility::update();
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
/**
static bool atapp_discovery_equal_ignore_order(
    const ::ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<std::string> &l,
    const ::ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<std::string> &r) {
  if (l.size() != r.size()) {
    return false;
  }

  std::unordered_set<std::string> rvalues;
  rvalues.reserve(static_cast<size_t>(r.size()));
  for (auto &rv : r) {
    rvalues.insert(rv);
  }

  for (auto &lv : l) {
    if (rvalues.end() == rvalues.find(lv)) {
      return false;
    }
  }

  return true;
}

static bool atapp_discovery_equal(
    const ::ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atbus_subnet_range> &l,
    const ::ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atbus_subnet_range> &r) {
  if (l.size() != r.size()) {
    return false;
  }

  std::vector<const atapp::protocol::atbus_subnet_range *> ls;
  std::vector<const atapp::protocol::atbus_subnet_range *> rs;
  ls.reserve(static_cast<size_t>(l.size()));
  rs.reserve(static_cast<size_t>(r.size()));
  for (int i = 0; i < l.size(); ++i) {
    ls.push_back(&l.Get(i));
    rs.push_back(&r.Get(i));
  }
  std::sort(ls.begin(), ls.end(),
            [](const atapp::protocol::atbus_subnet_range *cl, const atapp::protocol::atbus_subnet_range *cr) {
              if (cl->id_prefix() == cr->id_prefix()) {
                return cl->mask_bits() < cr->mask_bits();
              }
              return cl->id_prefix() < cr->id_prefix();
            });

  for (size_t i = 0; i < ls.size(); ++i) {
    if (ls[i]->id_prefix() != rs[i]->id_prefix() || ls[i]->mask_bits() != rs[i]->mask_bits()) {
      return false;
    }
  }

  return true;
}

static bool atapp_discovery_equal(const atapp::protocol::atapp_gateway &l, const atapp::protocol::atapp_gateway &r) {
  if (l.address().size() != r.address().size()) {
    return false;
  }
  if (l.match_hosts().size() != r.match_hosts().size()) {
    return false;
  }
  if (l.match_namespaces().size() != r.match_namespaces().size()) {
    return false;
  }
  if (l.match_labels().size() != r.match_labels().size()) {
    return false;
  }

  if (l.address() != r.address()) {
    return false;
  }

  if (!l.match_hosts().empty()) {
    if (atapp_discovery_equal_ignore_order(l.match_hosts(), r.match_hosts())) {
      return false;
    }
  }

  if (!l.match_namespaces().empty()) {
    if (atapp_discovery_equal_ignore_order(l.match_namespaces(), r.match_namespaces())) {
      return false;
    }
  }

  if (!l.match_labels().empty()) {
    for (auto &rkv : r.match_labels()) {
      auto iter = l.match_labels().find(rkv.first);
      if (iter == l.match_labels().end()) {
        return false;
      }
      if (iter->second != rkv.second) {
        return false;
      }
    }
  }

  return true;
}

static bool atapp_discovery_equal(
    const ::ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atapp_gateway> &l,
    const ::ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atapp_gateway> &r) {
  if (l.size() != r.size()) {
    return false;
  }

  std::vector<const atapp::protocol::atapp_gateway *> ls;
  std::vector<const atapp::protocol::atapp_gateway *> rs;
  ls.reserve(static_cast<size_t>(l.size()));
  rs.reserve(static_cast<size_t>(r.size()));
  for (int i = 0; i < l.size(); ++i) {
    ls.push_back(&l.Get(i));
    rs.push_back(&r.Get(i));
  }
  std::sort(ls.begin(), ls.end(),
            [](const atapp::protocol::atapp_gateway *cl, const atapp::protocol::atapp_gateway *cr) {
              return cl->address() < cr->address();
            });

  for (size_t i = 0; i < ls.size(); ++i) {
    if (!atapp_discovery_equal(*ls[i], *rs[i])) {
      return false;
    }
  }

  return true;
}
**/

static bool atapp_discovery_equal(const atapp::protocol::atapp_area &l, const atapp::protocol::atapp_area &r) {
  if (l.zone_id() != r.zone_id()) {
    return false;
  }
  if (l.region().size() != r.region().size()) {
    return false;
  }
  if (l.district().size() != r.district().size()) {
    return false;
  }
  if (l.region() != r.region()) {
    return false;
  }
  if (l.district() != r.district()) {
    return false;
  }

  return false;
}

}  // namespace detail

LIBATAPP_MACRO_API etcd_module::etcd_module()
    : etcd_ctx_enabled_(false),
      maybe_update_internal_keepalive_value_(true),
      maybe_update_internal_keepalive_area_(false),
      maybe_update_internal_keepalive_metadata_(false),
      watcher_snapshot_index_allocator_(0) {
  tick_next_timepoint_ = util::time::time_utility::sys_now();
  tick_interval_ = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(128));

  last_etcd_event_header_.cluster_id = 0;
  last_etcd_event_header_.member_id = 0;
  last_etcd_event_header_.raft_term = 0;
  last_etcd_event_header_.revision = 0;
}

LIBATAPP_MACRO_API etcd_module::~etcd_module() { reset(); }

LIBATAPP_MACRO_API void etcd_module::reset() {
  conf_path_cache_.clear();
  custom_data_.clear();
  if (cleanup_request_) {
    cleanup_request_->set_priv_data(nullptr);
    cleanup_request_->set_on_complete(nullptr);
    cleanup_request_->stop();
    cleanup_request_.reset();
  }

  etcd_ctx_.reset();

  if (curl_multi_) {
    util::network::http_request::destroy_curl_multi(curl_multi_);
  }
}

LIBATAPP_MACRO_API int etcd_module::init() {
  // init curl
  int res = curl_global_init(CURL_GLOBAL_ALL);
  if (res) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "init cURL failed, errcode: {}", res);
    return -1;
  }

  util::network::http_request::curl_share_options share_options;
  util::network::http_request::curl_multi_options multi_options;
  multi_options.ev_loop = get_app()->get_bus_node()->get_evloop();
  // Share DNS cache, connection, TLS sessions and etc.
  util::network::http_request::create_curl_share(share_options, multi_options.share_context);
  util::network::http_request::create_curl_multi(get_app()->get_bus_node()->get_evloop(), curl_multi_);
  if (!curl_multi_) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create curl multi instance failed.");
    return -1;
  }

  const atapp::protocol::atapp_etcd &conf = get_configure();
  if (!etcd_ctx_enabled_) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "etcd disabled, start single mode");
    return res;
  }

  if (etcd_ctx_.get_conf_hosts().empty()) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "etcd host not found, start single mode");
    return res;
  }

  etcd_ctx_.init(curl_multi_);

  // generate keepalives
  res = init_keepalives();
  if (res < 0) {
    return res;
  }
  // generate watchers data
  res = init_watchers();
  if (res < 0) {
    return res;
  }

  // Setup for first, we must check if all resource available.
  bool is_failed = false;

  // setup timer for timeout
  uv_timer_t tick_timer;
  detail::init_timer_data_type tick_timer_data;
  uv_timer_init(get_app()->get_bus_node()->get_evloop(), &tick_timer);
  tick_timer_data.cluster = &etcd_ctx_;
  tick_timer_data.timeout = std::chrono::system_clock::now();
  tick_timer_data.timeout += std::chrono::seconds(conf.init().timeout().seconds());
  tick_timer_data.timeout += std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::nanoseconds(conf.init().timeout().nanos()));
  tick_timer.data = &tick_timer_data;

  uv_timer_start(&tick_timer, detail::init_timer_tick_callback, 128, 128);

  int ticks = 0;
  while (false == is_failed && std::chrono::system_clock::now() <= tick_timer_data.timeout && nullptr != get_app()) {
    util::time::time_utility::update();
    etcd_ctx_.tick();
    ++ticks;

    size_t run_count = 0;
    // Check keepalives
    for (std::list<etcd_keepalive::ptr_t>::iterator iter = inner_keepalive_actors_.begin();
         iter != inner_keepalive_actors_.end(); ++iter) {
      if ((*iter)->is_check_run()) {
        if (!(*iter)->is_check_passed()) {
          LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "etcd_keepalive lock {} failed.", (*iter)->get_path());
          is_failed = true;
        }

        ++run_count;
      }
    }

    // Check watchers

    // 全部成功或任意失败则退出
    if (is_failed || run_count >= inner_keepalive_actors_.size()) {
      break;
    }

    uv_run(get_app()->get_bus_node()->get_evloop(), UV_RUN_ONCE);

    // 任意重试次数过多则失败退出
    for (std::list<etcd_keepalive::ptr_t>::iterator iter = inner_keepalive_actors_.begin();
         iter != inner_keepalive_actors_.end(); ++iter) {
      if ((*iter)->get_check_times() >= ETCD_MODULE_STARTUP_RETRY_TIMES ||
          etcd_ctx_.get_stats().continue_error_requests > ETCD_MODULE_STARTUP_RETRY_TIMES) {
        size_t retry_times = (*iter)->get_check_times();
        if (etcd_ctx_.get_stats().continue_error_requests > retry_times) {
          retry_times = etcd_ctx_.get_stats().continue_error_requests > retry_times;
        }
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_,
                                              "etcd_keepalive request {} for {} times (with {} ticks) failed.",
                                              (*iter)->get_path(), retry_times, ticks);
        is_failed = true;
        break;
      }
    }

    if (is_failed) {
      break;
    }
  }

  if (std::chrono::system_clock::now() > tick_timer_data.timeout) {
    is_failed = true;
    for (std::list<etcd_keepalive::ptr_t>::iterator iter = inner_keepalive_actors_.begin();
         iter != inner_keepalive_actors_.end(); ++iter) {
      size_t retry_times = (*iter)->get_check_times();
      if (etcd_ctx_.get_stats().continue_error_requests > retry_times) {
        retry_times = etcd_ctx_.get_stats().continue_error_requests;
      }
      if ((*iter)->is_check_passed()) {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_WARNING(
            etcd_ctx_, "etcd_keepalive request {} timeout, retry {} times (with {} ticks), check passed, has data: {}.",
            (*iter)->get_path(), retry_times, ticks, (*iter)->has_data() ? "true" : "false");
      } else {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
            etcd_ctx_,
            "etcd_keepalive request {} timeout, retry {} times (with {} ticks), check unpassed, has data: {}.",
            (*iter)->get_path(), retry_times, ticks, (*iter)->has_data() ? "true" : "false");
      }
    }
  }

  // close timer for timeout
  uv_timer_stop(&tick_timer);
  uv_close((uv_handle_t *)&tick_timer, detail::init_timer_closed_callback);
  while (tick_timer.data) {
    uv_run(get_app()->get_bus_node()->get_evloop(), UV_RUN_ONCE);
  }

  // 初始化失败则回收资源
  if (is_failed) {
    stop();
    reset();
    return -1;
  }

  return res;
}

void etcd_module::update_keepalive_value() {
  if (!(maybe_update_internal_keepalive_value_ || maybe_update_internal_keepalive_area_ ||
        maybe_update_internal_keepalive_metadata_) ||
      nullptr == get_app()) {
    return;
  }

  std::string new_value;
  node_info_t ni;
  get_app()->pack(ni.node_discovery);

  bool has_changed = maybe_update_internal_keepalive_value_;
  maybe_update_internal_keepalive_value_ = false;
  if (maybe_update_internal_keepalive_area_) {
    if (!detail::atapp_discovery_equal(ni.node_discovery.area(), last_submmited_discovery_data_area_)) {
      has_changed = true;
      last_submmited_discovery_data_area_ = ni.node_discovery.area();
    }
  }
  maybe_update_internal_keepalive_area_ = false;
  if (maybe_update_internal_keepalive_metadata_) {
    if (!etcd_discovery_set::metadata_equal_type()(ni.node_discovery.metadata(),
                                                   last_submmited_discovery_data_metadata_)) {
      has_changed = true;
      last_submmited_discovery_data_metadata_ = ni.node_discovery.metadata();
    }
  }
  maybe_update_internal_keepalive_metadata_ = false;

  if (!has_changed) {
    return;
  }

  pack(ni, new_value);
  if (new_value != inner_keepalive_value_) {
    inner_keepalive_value_.swap(new_value);

    for (std::list<etcd_keepalive::ptr_t>::iterator iter = inner_keepalive_actors_.begin();
         iter != inner_keepalive_actors_.end(); ++iter) {
      if (*iter) {
        (*iter)->set_value(inner_keepalive_value_);
      }
    }
  }
}

int etcd_module::init_keepalives() {
  const atapp::protocol::atapp_etcd &conf = get_configure();
  update_keepalive_value();

  if (conf.report_alive().by_id()) {
    atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(inner_keepalive_value_, get_by_id_path());
    if (!actor) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_keepalive for by_id index failed.");
      return -1;
    }

    inner_keepalive_actors_.push_back(actor);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_keepalive {} for by_id index {} success",
                                         reinterpret_cast<const void *>(actor.get()), get_by_id_path());
  }

  if (conf.report_alive().by_type()) {
    atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(inner_keepalive_value_, get_by_type_id_path());
    if (!actor) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_keepalive for by_type_id index failed.");
      return -1;
    }
    inner_keepalive_actors_.push_back(actor);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_keepalive {} for by_type_id index {} success",
                                         reinterpret_cast<const void *>(actor.get()), get_by_type_id_path());

    actor = add_keepalive_actor(inner_keepalive_value_, get_by_type_name_path());
    if (!actor) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_keepalive for by_type_name index failed.");
      return -1;
    }
    inner_keepalive_actors_.push_back(actor);

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_keepalive {} for by_type_name index {} success",
                                         reinterpret_cast<const void *>(actor.get()), get_by_type_name_path());
  }

  if (conf.report_alive().by_name()) {
    atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(inner_keepalive_value_, get_by_name_path());
    if (!actor) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_keepalive for by_name index failed.");
      return -1;
    }

    inner_keepalive_actors_.push_back(actor);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_keepalive {} for by_name index {} success",
                                         reinterpret_cast<const void *>(actor.get()), get_by_name_path());
  }

  for (int i = 0; i < conf.report_alive().by_tag_size(); ++i) {
    const std::string &tag_name = conf.report_alive().by_tag(i);

    if (tag_name.empty()) {
      continue;
    }

    atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(inner_keepalive_value_, get_by_tag_path(tag_name));
    if (!actor) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_keepalive for by_tag {} index failed.", tag_name);
      return -1;
    }

    inner_keepalive_actors_.push_back(actor);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_keepalive {} for by_tag index {} success",
                                         reinterpret_cast<const void *>(actor.get()), get_by_tag_path(tag_name));
  }

  return 0;
}

int etcd_module::init_watchers() {
  const atapp::protocol::atapp_etcd &conf = get_configure();

  // setup configured watchers
  if (conf.watcher().by_name()) {
    add_watcher_by_name(nullptr);
    return 0;
  }

  if (conf.watcher().by_id()) {
    add_watcher_by_id(nullptr);
    return 0;
  }

  // 内置有按type id或按type name watch
  for (int i = 0; i < conf.watcher().by_type_id_size(); ++i) {
    uint64_t type_id = conf.watcher().by_type_id(i);
    if (0 == type_id) {
      continue;
    }
    add_watcher_by_type_id(type_id, nullptr);
  }
  for (int i = 0; i < conf.watcher().by_type_name_size(); ++i) {
    if (conf.watcher().by_type_name(i).empty()) {
      continue;
    }
    add_watcher_by_type_name(conf.watcher().by_type_name(i), nullptr);
  }

  // 内置有按tag watch
  for (int i = 0; i < conf.watcher().by_tag_size(); ++i) {
    if (conf.watcher().by_tag(i).empty()) {
      continue;
    }
    add_watcher_by_type_name(conf.watcher().by_tag(i), nullptr);
  }

  return 0;
}

LIBATAPP_MACRO_API int etcd_module::reload() {
  // load configure
  conf_path_cache_.clear();
  const atapp::protocol::atapp_etcd &conf = get_configure();

  // load logger
  util::log::log_wrapper::ptr_t logger;
  util::log::log_formatter::level_t::type startup_level = util::log::log_formatter::level_t::LOG_LW_DISABLED;
  do {
    atapp::protocol::atapp_log etcd_log_conf;
    get_app()->parse_log_configures_into(etcd_log_conf, std::vector<gsl::string_view>{"atapp", "etcd", "log"},
                                         "ATAPP_ETCD_LOG");
    if (etcd_log_conf.category_size() <= 0) {
      break;
    }

    startup_level = util::log::log_formatter::get_level_by_name(conf.log().startup_level().c_str());
    if (startup_level <= util::log::log_formatter::level_t::LOG_LW_DISABLED &&
        util::log::log_formatter::get_level_by_name(etcd_log_conf.level().c_str()) <=
            util::log::log_formatter::level_t::LOG_LW_DISABLED) {
      break;
    }

    logger = util::log::log_wrapper::create_user_logger();
    if (!logger) {
      break;
    }
    logger->init(util::log::log_formatter::get_level_by_name(etcd_log_conf.level().c_str()));
    get_app()->setup_logger(*logger, etcd_log_conf.level(), etcd_log_conf.category(0));
  } while (false);
  etcd_ctx_.set_logger(logger, startup_level);

  // etcd context configure
  {
    std::vector<std::string> conf_hosts;
    conf_hosts.reserve(static_cast<size_t>(conf.hosts_size()));
    for (int i = 0; i < conf.hosts_size(); ++i) {
      conf_hosts.push_back(conf.hosts(i));
    }
    if (!conf_hosts.empty()) {
      etcd_ctx_.set_conf_hosts(conf_hosts);
    }
  }

  etcd_ctx_.set_conf_authorization(conf.authorization());
  etcd_ctx_.set_conf_http_request_timeout(detail::convert_to_chrono(conf.request().timeout(), 10000));
  etcd_ctx_.set_conf_http_initialization_timeout(
      detail::convert_to_chrono(conf.request().initialization_timeout(), 3000));
  etcd_ctx_.set_conf_http_connect_timeout(detail::convert_to_chrono(conf.request().connect_timeout(), 0));
  etcd_ctx_.set_conf_dns_cache_timeout(detail::convert_to_chrono(conf.request().dns_cache_timeout(), 0));
  etcd_ctx_.set_conf_dns_servers(conf.request().dns_servers());
  etcd_ctx_.set_conf_etcd_members_auto_update_hosts(conf.cluster().auto_update());
  etcd_ctx_.set_conf_etcd_members_update_interval(detail::convert_to_chrono(conf.cluster().update_interval(), 300000));
  etcd_ctx_.set_conf_etcd_members_retry_interval(detail::convert_to_chrono(conf.cluster().retry_interval(), 60000));

  etcd_ctx_.set_conf_keepalive_timeout(detail::convert_to_chrono(conf.keepalive().timeout(), 16000));
  etcd_ctx_.set_conf_keepalive_interval(detail::convert_to_chrono(conf.keepalive().ttl(), 5000));
  etcd_ctx_.set_conf_keepalive_retry_interval(detail::convert_to_chrono(conf.keepalive().retry_interval(), 3000));

  // HTTP
  if (!conf.http().user_agent().empty()) {
    etcd_ctx_.set_conf_user_agent(conf.http().user_agent());
  }
  if (!conf.http().proxy().empty()) {
    etcd_ctx_.set_conf_proxy(conf.http().proxy());
  }
  if (!conf.http().no_proxy().empty()) {
    etcd_ctx_.set_conf_no_proxy(conf.http().no_proxy());
  }
  if (!conf.http().proxy_user_name().empty()) {
    etcd_ctx_.set_conf_proxy_user_name(conf.http().proxy_user_name());
  }
  if (!conf.http().proxy_password().empty()) {
    etcd_ctx_.set_conf_proxy_password(conf.http().proxy_password());
  }

  etcd_ctx_.set_conf_http_debug_mode(conf.http().debug());

  // SSL configure
  etcd_ctx_.set_conf_ssl_enable_alpn(conf.ssl().enable_alpn());
  etcd_ctx_.set_conf_ssl_verify_peer(conf.ssl().verify_peer());
  do {
    const std::string &ssl_version = conf.ssl().ssl_min_version();
    if (ssl_version.empty()) {
      break;
    }
    if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.3", 7) ||
        0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv13", 6)) {
      etcd_ctx_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::TLS_V13);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.2", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv12", 6)) {
      etcd_ctx_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::TLS_V12);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.1", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv11", 6)) {
      etcd_ctx_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::TLS_V11);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1", 5) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv1.0", 7) ||
               0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "TLSv10", 6)) {
      etcd_ctx_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::TLS_V10);
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(ssl_version.c_str(), "SSLv3", 5)) {
      etcd_ctx_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::SSL3);
    } else {
      etcd_ctx_.set_conf_ssl_min_version(etcd_cluster::ssl_version_t::DISABLED);
    }
  } while (false);

  if (!conf.ssl().ssl_client_cert().empty()) {
    etcd_ctx_.set_conf_ssl_client_cert(conf.ssl().ssl_client_cert());
  }

  if (!conf.ssl().ssl_client_cert_type().empty()) {
    etcd_ctx_.set_conf_ssl_client_cert_type(conf.ssl().ssl_client_cert_type());
  }

  if (!conf.ssl().ssl_client_key().empty()) {
    etcd_ctx_.set_conf_ssl_client_key(conf.ssl().ssl_client_key());
  }

  if (!conf.ssl().ssl_client_key_type().empty()) {
    etcd_ctx_.set_conf_ssl_client_key_type(conf.ssl().ssl_client_key_type());
  }

  if (!conf.ssl().ssl_client_key_passwd().empty()) {
    etcd_ctx_.set_conf_ssl_client_key_passwd(conf.ssl().ssl_client_key_passwd());
  }

  if (!conf.ssl().ssl_ca_cert().empty()) {
    etcd_ctx_.set_conf_ssl_ca_cert(conf.ssl().ssl_ca_cert());
  }

  if (!conf.ssl().ssl_proxy_cert().empty()) {
    etcd_ctx_.set_conf_ssl_proxy_cert(conf.ssl().ssl_proxy_cert());
  }

  if (!conf.ssl().ssl_proxy_cert_type().empty()) {
    etcd_ctx_.set_conf_ssl_proxy_cert_type(conf.ssl().ssl_proxy_cert_type());
  }

  if (!conf.ssl().ssl_proxy_key().empty()) {
    etcd_ctx_.set_conf_ssl_proxy_key(conf.ssl().ssl_proxy_key());
  }

  if (!conf.ssl().ssl_proxy_key_type().empty()) {
    etcd_ctx_.set_conf_ssl_proxy_key_type(conf.ssl().ssl_proxy_key_type());
  }

  if (!conf.ssl().ssl_proxy_key_passwd().empty()) {
    etcd_ctx_.set_conf_ssl_proxy_key_passwd(conf.ssl().ssl_proxy_key_passwd());
  }

  if (!conf.ssl().ssl_proxy_ca_cert().empty()) {
    etcd_ctx_.set_conf_ssl_proxy_ca_cert(conf.ssl().ssl_proxy_ca_cert());
  }

  if (!conf.ssl().ssl_cipher_list().empty()) {
    etcd_ctx_.set_conf_ssl_cipher_list(conf.ssl().ssl_cipher_list());
  }

  if (!conf.ssl().ssl_cipher_list_tls13().empty()) {
    etcd_ctx_.set_conf_ssl_cipher_list_tls13(conf.ssl().ssl_cipher_list_tls13());
  }

  etcd_ctx_enabled_ = conf.enable();
  tick_interval_ = std::chrono::duration_cast<std::chrono::system_clock::duration>(
      std::chrono::seconds(conf.init().tick_interval().seconds()) +
      std::chrono::nanoseconds(conf.init().tick_interval().nanos()));
  if (tick_interval_ < std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(32))) {
    tick_interval_ = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(128));
  }
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::stop() {
  if (!cleanup_request_ && !etcd_ctx_.check_flag(etcd_cluster::flag_t::CLOSING)) {
    bool revoke_lease = true;
    if (get_app() && get_app()->is_current_upgrade_mode()) {
      revoke_lease = false;
    }

    cleanup_request_ = etcd_ctx_.close(false, revoke_lease);

    if (cleanup_request_ && cleanup_request_->is_running()) {
      cleanup_request_->set_priv_data(this);
      cleanup_request_->set_on_complete(http_callback_on_etcd_closed);
    }
    reset_inner_watchers_and_keepalives();
  }

  if (cleanup_request_ && cleanup_request_->is_running()) {
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

LIBATAPP_MACRO_API const char *etcd_module::name() const { return "atapp: etcd module"; }

LIBATAPP_MACRO_API int etcd_module::tick() {
  atapp::app *owner = get_app();
  if (nullptr == owner) {
    return 0;
  }

  // Slow down the tick interval of etcd module, it require http request which is very slow compared to atbus
  if (tick_next_timepoint_ >= owner->get_last_tick_time()) {
    return 0;
  }
  tick_next_timepoint_ = owner->get_last_tick_time() + tick_interval_;

  // single mode
  if (etcd_ctx_.get_conf_hosts().empty() || !etcd_ctx_enabled_) {
    // If it's initializing, is_available() will return false
    if (!cleanup_request_ && etcd_ctx_.is_available()) {
      bool revoke_lease = true;
      if (owner->is_current_upgrade_mode()) {
        revoke_lease = false;
      }

      cleanup_request_ = etcd_ctx_.close(false, revoke_lease);
      reset_inner_watchers_and_keepalives();
    }

    return 0;
  }

  // previous closing not finished, wait for it
  if (cleanup_request_ || owner->is_closing()) {
    return etcd_ctx_.tick();
  }

  // first startup when reloaded
  if (!curl_multi_) {
    int res = init();
    if (res < 0) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "initialize etcd failed, res: {}", res);
      owner->stop();
      return res;
    }
  } else if (etcd_ctx_.check_flag(etcd_cluster::flag_t::CLOSING)) {  // Already stoped and restart etcd_ctx_
    etcd_ctx_.init(curl_multi_);
    // generate keepalives
    int res = init_keepalives();
    if (res < 0) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "reinitialize etcd keepalives failed, res: {}", res);
    }
    // generate watchers data
    res = init_watchers();
    if (res < 0) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "reinitialize etcd watchers failed, res: {}", res);
    }
  }

  int ret = etcd_ctx_.tick();

  if ((maybe_update_internal_keepalive_value_ || maybe_update_internal_keepalive_area_ ||
       maybe_update_internal_keepalive_metadata_) &&
      etcd_ctx_.check_flag(etcd_cluster::flag_t::RUNNING)) {
    update_keepalive_value();
  }

  return ret;
}

LIBATAPP_MACRO_API const std::string &etcd_module::get_conf_custom_data() const { return custom_data_; }
LIBATAPP_MACRO_API void etcd_module::set_conf_custom_data(const std::string &v) { custom_data_ = v; }

LIBATAPP_MACRO_API bool etcd_module::is_etcd_enabled() const {
  return etcd_ctx_enabled_ && !etcd_ctx_.get_conf_hosts().empty();
}
LIBATAPP_MACRO_API void etcd_module::enable_etcd() { etcd_ctx_enabled_ = true; }
LIBATAPP_MACRO_API void etcd_module::disable_etcd() { etcd_ctx_enabled_ = false; }

LIBATAPP_MACRO_API void etcd_module::set_maybe_update_keepalive_value() {
  maybe_update_internal_keepalive_value_ = true;
}

LIBATAPP_MACRO_API void etcd_module::set_maybe_update_keepalive_area() { maybe_update_internal_keepalive_area_ = true; }

LIBATAPP_MACRO_API void etcd_module::set_maybe_update_keepalive_metadata() {
  maybe_update_internal_keepalive_metadata_ = true;
}

LIBATAPP_MACRO_API const util::network::http_request::curl_m_bind_ptr_t &etcd_module::get_shared_curl_multi_context()
    const {
  return curl_multi_;
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_id_path() const {
  const atapp::app *owner = get_app();
  if (nullptr == owner) {
    return std::string();
  }

  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_ID_DIR, owner->get_app_name(),
                                  owner->get_id());
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_type_id_path() const {
  const atapp::app *owner = get_app();
  if (nullptr == owner) {
    return std::string();
  }

  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_TYPE_ID_DIR,
                                  owner->get_type_id(), owner->get_app_name(), owner->get_id());
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_type_name_path() const {
  const atapp::app *owner = get_app();
  if (nullptr == owner) {
    return std::string();
  }

  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_TYPE_NAME_DIR,
                                  owner->get_type_name(), owner->get_app_name(), owner->get_id());
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_name_path() const {
  const atapp::app *owner = get_app();
  if (nullptr == owner) {
    return std::string();
  }

  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_NAME_DIR, owner->get_app_name(),
                                  owner->get_id());
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_tag_path(const std::string &tag_name) const {
  const atapp::app *owner = get_app();
  if (nullptr == owner) {
    return std::string();
  }

  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_TAG_DIR, tag_name,
                                  owner->get_app_name(), owner->get_id());
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_id_watcher_path() const {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}", get_configure_path(), ETCD_MODULE_BY_ID_DIR);
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_type_id_watcher_path(uint64_t type_id) const {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_ID_DIR, type_id);
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_type_name_watcher_path(const std::string &type_name) const {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_NAME_DIR, type_name);
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_name_watcher_path() const {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}", get_configure_path(), ETCD_MODULE_BY_NAME_DIR);
}

LIBATAPP_MACRO_API std::string etcd_module::get_by_tag_watcher_path(const std::string &tag_name) const {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}", get_configure_path(), ETCD_MODULE_BY_TAG_DIR, tag_name);
}

LIBATAPP_MACRO_API int etcd_module::add_watcher_by_id(watcher_list_callback_t fn) {
  if (!fn) {
    const atapp::protocol::atapp_etcd &conf = get_configure();
    if (!conf.watcher().by_id()) {
      return EN_ATBUS_ERR_PARAMS;
    }
  }

  if (!inner_watcher_by_id_) {
    std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}{}", get_configure_path(), ETCD_MODULE_BY_ID_DIR);

    inner_watcher_by_id_ = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
    if (!inner_watcher_by_id_) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_watcher by_id failed.");
      return EN_ATBUS_ERR_MALLOC;
    }

    inner_watcher_by_id_->set_conf_request_timeout(
        detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
    inner_watcher_by_id_->set_conf_retry_interval(
        detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
    inner_watcher_by_name_->set_conf_get_request_timeout(
        detail::convert_to_chrono(get_configure().watcher().get_request_timeout(), 180000));
    inner_watcher_by_name_->set_conf_startup_random_delay_min(
        detail::convert_to_chrono(get_configure().watcher().startup_random_delay_min(), 0));
    inner_watcher_by_name_->set_conf_startup_random_delay_max(
        detail::convert_to_chrono(get_configure().watcher().startup_random_delay_max(), 0));
    etcd_ctx_.add_watcher(inner_watcher_by_id_);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_watcher for by_id index {} success", watch_path);

    inner_watcher_by_id_->set_evt_handle(
        watcher_callback_list_wrapper_t(*this, watcher_by_id_callbacks_, ++watcher_snapshot_index_allocator_));
  }

  if (fn) {
    watcher_by_id_callbacks_.push_back(fn);
  }
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::add_watcher_by_type_id(uint64_t type_id, watcher_one_callback_t fn) {
  if (!fn) {
    return EN_ATBUS_ERR_PARAMS;
  }

  // generate watch data
  std::string watch_path =
      LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_ID_DIR, type_id);

  atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
  if (!p) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_watcher by_type_id failed.");
    return EN_ATBUS_ERR_MALLOC;
  }

  p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
  p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
  p->set_conf_get_request_timeout(detail::convert_to_chrono(get_configure().watcher().get_request_timeout(), 180000));
  p->set_conf_startup_random_delay_min(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_min(), 0));
  p->set_conf_startup_random_delay_max(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_max(), 0));
  etcd_ctx_.add_watcher(p);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_watcher for by_type_id index {} success", watch_path);

  p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn, ++watcher_snapshot_index_allocator_));
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::add_watcher_by_type_name(const std::string &type_name, watcher_one_callback_t fn) {
  // generate watch data
  std::string watch_path =
      LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_NAME_DIR, type_name);

  atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
  if (!p) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_watcher by_type_name failed.");
    return EN_ATBUS_ERR_MALLOC;
  }

  p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
  p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
  p->set_conf_get_request_timeout(detail::convert_to_chrono(get_configure().watcher().get_request_timeout(), 180000));
  p->set_conf_startup_random_delay_min(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_min(), 0));
  p->set_conf_startup_random_delay_max(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_max(), 0));
  etcd_ctx_.add_watcher(p);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_watcher for by_type_name index {} success", watch_path);

  p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn, ++watcher_snapshot_index_allocator_));
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::add_watcher_by_name(watcher_list_callback_t fn) {
  if (!fn) {
    const atapp::protocol::atapp_etcd &conf = get_configure();
    if (!conf.watcher().by_name()) {
      return EN_ATBUS_ERR_PARAMS;
    }
  }

  if (!inner_watcher_by_name_) {
    // generate watch data
    std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}{}", get_configure_path(), ETCD_MODULE_BY_NAME_DIR);

    inner_watcher_by_name_ = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
    if (!inner_watcher_by_name_) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_watcher by_name failed.");
      return EN_ATBUS_ERR_MALLOC;
    }

    inner_watcher_by_name_->set_conf_request_timeout(
        detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
    inner_watcher_by_name_->set_conf_retry_interval(
        detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
    inner_watcher_by_name_->set_conf_get_request_timeout(
        detail::convert_to_chrono(get_configure().watcher().get_request_timeout(), 180000));
    inner_watcher_by_name_->set_conf_startup_random_delay_min(
        detail::convert_to_chrono(get_configure().watcher().startup_random_delay_min(), 0));
    inner_watcher_by_name_->set_conf_startup_random_delay_max(
        detail::convert_to_chrono(get_configure().watcher().startup_random_delay_max(), 0));
    etcd_ctx_.add_watcher(inner_watcher_by_name_);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_watcher for by_name index {} success", watch_path);

    inner_watcher_by_name_->set_evt_handle(
        watcher_callback_list_wrapper_t(*this, watcher_by_name_callbacks_, ++watcher_snapshot_index_allocator_));
  }

  if (fn) {
    watcher_by_name_callbacks_.push_back(fn);
  }
  return 0;
}

LIBATAPP_MACRO_API int etcd_module::add_watcher_by_tag(const std::string &tag_name, watcher_one_callback_t fn) {
  // generate watch data
  std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}", get_configure_path(), ETCD_MODULE_BY_TAG_DIR, tag_name);

  atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
  if (!p) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_watcher by_tag failed.");
    return EN_ATBUS_ERR_MALLOC;
  }

  p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
  p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
  p->set_conf_get_request_timeout(detail::convert_to_chrono(get_configure().watcher().get_request_timeout(), 180000));
  p->set_conf_startup_random_delay_min(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_min(), 0));
  p->set_conf_startup_random_delay_max(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_max(), 0));

  etcd_ctx_.add_watcher(p);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_watcher for by_tag index {} success", watch_path);

  p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn, ++watcher_snapshot_index_allocator_));
  return 0;
}

LIBATAPP_MACRO_API atapp::etcd_watcher::ptr_t etcd_module::add_watcher_by_custom_path(const std::string &custom_path,
                                                                                      watcher_one_callback_t fn) {
  const std::string &configure_path = get_configure_path();
  if (custom_path.size() < configure_path.size() ||
      0 != UTIL_STRFUNC_STRNCMP(custom_path.c_str(), configure_path.c_str(), configure_path.size())) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_,
                                          "create etcd_watcher by custom path {} failed. path must has prefix of {}.",
                                          custom_path, configure_path);
    return nullptr;
  }

  atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, custom_path, "+1");
  if (!p) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
        etcd_ctx_, "create etcd_watcher by custom path {} failed. malloc etcd_watcher failed.", custom_path);
    return nullptr;
  }

  p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
  p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
  p->set_conf_get_request_timeout(detail::convert_to_chrono(get_configure().watcher().get_request_timeout(), 180000));
  p->set_conf_startup_random_delay_min(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_min(), 0));
  p->set_conf_startup_random_delay_max(
      detail::convert_to_chrono(get_configure().watcher().startup_random_delay_max(), 0));

  p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn, ++watcher_snapshot_index_allocator_));
  if (etcd_ctx_.add_watcher(p)) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "create etcd_watcher by custom path {} success", custom_path);
  } else {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(etcd_ctx_, "add etcd_watcher by custom path {} failed", custom_path);
    p->set_evt_handle(nullptr);
    p.reset();
  }

  return p;
}

LIBATAPP_MACRO_API const ::atapp::etcd_cluster &etcd_module::get_raw_etcd_ctx() const { return etcd_ctx_; }
LIBATAPP_MACRO_API ::atapp::etcd_cluster &etcd_module::get_raw_etcd_ctx() { return etcd_ctx_; }

LIBATAPP_MACRO_API const ::atapp::etcd_response_header &etcd_module::get_last_etcd_event_header() const {
  return last_etcd_event_header_;
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &etcd_module::get_configure() const {
  return get_app()->get_origin_configure().etcd();
}

LIBATAPP_MACRO_API const std::string &etcd_module::get_configure_path() const {
  if (conf_path_cache_.empty()) {
    etcd_module *self = const_cast<etcd_module *>(this);
    self->conf_path_cache_ = get_configure().path();

    if (!self->conf_path_cache_.empty()) {
      size_t last_idx = self->conf_path_cache_.size() - 1;
      if (self->conf_path_cache_[last_idx] != '/' && self->conf_path_cache_[last_idx] != '\\') {
        self->conf_path_cache_ += '/';
      }
    } else {
      self->conf_path_cache_ = std::string("/");
    }
  }

  return conf_path_cache_;
}

LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t etcd_module::add_keepalive_actor(std::string &val,
                                                                                 const std::string &node_path) {
  atapp::etcd_keepalive::ptr_t ret;
  if (nullptr == get_app()) {
    return ret;
  }

  if (val.empty()) {
    node_info_t ni;
    get_app()->pack(ni.node_discovery);
    pack(ni, val);
  }

  ret = atapp::etcd_keepalive::create(etcd_ctx_, node_path);
  if (!ret) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(etcd_ctx_, "create etcd_keepalive failed.");
    return ret;
  }

  ret->set_checker(val);
  ret->set_value(val);

  if (!etcd_ctx_.add_keepalive(ret)) {
    ret.reset();
  }

  return ret;
}

LIBATAPP_MACRO_API bool etcd_module::remove_keepalive_actor(const atapp::etcd_keepalive::ptr_t &keepalive) {
  if (!keepalive) {
    return false;
  }

  std::list<etcd_keepalive::ptr_t>::iterator inner_iter =
      std::find(inner_keepalive_actors_.begin(), inner_keepalive_actors_.end(), keepalive);
  if (inner_iter != inner_keepalive_actors_.end()) {
    inner_keepalive_actors_.erase(inner_iter);
  }

  return etcd_ctx_.remove_keepalive(keepalive);
}

LIBATAPP_MACRO_API etcd_module::node_event_callback_handle_t etcd_module::add_on_node_discovery_event(
    node_event_callback_t fn) {
  if (!fn) {
    return node_event_callbacks_.end();
  }

  return node_event_callbacks_.insert(node_event_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void etcd_module::remove_on_node_event(node_event_callback_handle_t &handle) {
  if (handle == node_event_callbacks_.end()) {
    return;
  }

  node_event_callbacks_.erase(handle);
  handle = node_event_callbacks_.end();
}

LIBATAPP_MACRO_API etcd_discovery_set &etcd_module::get_global_discovery() { return global_discovery_; }
LIBATAPP_MACRO_API const etcd_discovery_set &etcd_module::get_global_discovery() const { return global_discovery_; }

LIBATAPP_MACRO_API bool etcd_module::has_snapshot() const { return !watcher_snapshot_index_.empty(); }

LIBATAPP_MACRO_API etcd_module::snapshot_event_callback_handle_t etcd_module::add_on_load_snapshot(
    snapshot_event_callback_t fn) {
  if (!fn) {
    return on_load_snapshot_callbacks_.end();
  }

  return on_load_snapshot_callbacks_.insert(on_load_snapshot_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void etcd_module::remove_on_load_snapshot(snapshot_event_callback_handle_t &handle) {
  if (handle == on_load_snapshot_callbacks_.end()) {
    return;
  }

  on_load_snapshot_callbacks_.erase(handle);
  handle = on_load_snapshot_callbacks_.end();
}

LIBATAPP_MACRO_API etcd_module::snapshot_event_callback_handle_t etcd_module::add_on_snapshot_loaded(
    snapshot_event_callback_t fn) {
  if (!fn) {
    return on_snapshot_loaded_callbacks_.end();
  }

  return on_snapshot_loaded_callbacks_.insert(on_snapshot_loaded_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void etcd_module::remove_on_snapshot_loaded(snapshot_event_callback_handle_t &handle) {
  if (handle == on_snapshot_loaded_callbacks_.end()) {
    return;
  }

  on_snapshot_loaded_callbacks_.erase(handle);
  handle = on_snapshot_loaded_callbacks_.end();
}

bool etcd_module::unpack(node_info_t &out, const std::string &path, const std::string &json, bool reset_data) {
  if (reset_data) {
    out.node_discovery.Clear();
  }

  if (json.empty()) {
    size_t start_idx = 0;
    size_t last_minus = 0;
    for (size_t i = 0; i < path.size(); ++i) {
      if (path[i] == '/' || path[i] == '\\' || path[i] == ' ' || path[i] == '\t' || path[i] == '\r' ||
          path[i] == '\n') {
        start_idx = i + 1;
      } else if (path[i] == '-') {
        last_minus = i;
      }
    }

    // parse id from key if key is a number
    if (start_idx < path.size()) {
      if (last_minus + 1 < path.size() && last_minus >= start_idx) {
        out.node_discovery.set_id(util::string::to_int<uint64_t>(path.c_str() + last_minus + 1));
        out.node_discovery.set_name(path.substr(start_idx, last_minus - start_idx));
      } else {
        // old mode, only id on last segment
        out.node_discovery.set_id(util::string::to_int<uint64_t>(&path[start_idx]));
      }
    }
    return false;
  }

  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;

  if (!ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonStringToMessage(json, &out.node_discovery, options).ok()) {
    return false;
  }

  return true;
}

void etcd_module::pack(const node_info_t &src, std::string &json) {
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonPrintOptions options;
  options.add_whitespace = false;
  options.always_print_enums_as_ints = true;
  options.preserve_proto_field_names = true;
  if(!ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::MessageToJsonString(src.node_discovery, &json, options).ok()) {
    FWLOGERROR("etcd_module pack message to json failed");
  }
}

int etcd_module::http_callback_on_etcd_closed(util::network::http_request &req) {
  etcd_module *self = reinterpret_cast<etcd_module *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("etcd_module get request shouldn't has request without private data");
    return 0;
  }

  self->cleanup_request_.reset();

  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(self->etcd_ctx_, "Etcd revoke lease finished");

  // call stop to trigger stop process again.
  if (nullptr != self->get_app()) {
    self->get_app()->stop();
  }

  return 0;
}

static void _collect_old_nodes(etcd_module &mod, etcd_discovery_set::node_by_name_type &old_names,
                               etcd_discovery_set::node_by_id_type &old_ids) {
  old_ids.reserve(mod.get_global_discovery().get_sorted_nodes().size());
  old_names.reserve(mod.get_global_discovery().get_sorted_nodes().size());
  for (auto &node : mod.get_global_discovery().get_sorted_nodes()) {
    if (!node) {
      continue;
    }
    if (0 != node->get_discovery_info().id()) {
      old_ids[node->get_discovery_info().id()] = node;
    } else if (!node->get_discovery_info().name().empty()) {
      old_names[node->get_discovery_info().name()] = node;
    }
  }
}

static void _remove_old_node_index(const atapp::protocol::atapp_discovery &node_discovery,
                                   etcd_discovery_set::node_by_name_type &old_names,
                                   etcd_discovery_set::node_by_id_type &old_ids) {
  if (0 != node_discovery.id()) {
    old_ids.erase(node_discovery.id());
  }
  if (!node_discovery.name().empty()) {
    old_names.erase(node_discovery.name());
  }
}

void etcd_module::watcher_internal_access_t::cleanup_old_nodes(etcd_module &mod,
                                                               etcd_discovery_set::node_by_name_type &old_names,
                                                               etcd_discovery_set::node_by_id_type &old_ids) {
  for (auto &node : old_names) {
    etcd_module::node_info_t evt_node;
    evt_node.action = etcd_module::node_action_t::EN_NAT_DELETE;
    node.second->copy_key_to(evt_node.node_discovery);
    mod.update_inner_watcher_event(evt_node, node.second->get_version());
  }

  for (auto &node : old_ids) {
    etcd_module::node_info_t evt_node;
    evt_node.action = etcd_module::node_action_t::EN_NAT_DELETE;
    node.second->copy_key_to(evt_node.node_discovery);
    mod.update_inner_watcher_event(evt_node, node.second->get_version());
  }
}

etcd_module::watcher_callback_list_wrapper_t::watcher_callback_list_wrapper_t(etcd_module &m,
                                                                              std::list<watcher_list_callback_t> &cbks,
                                                                              int64_t index)
    : mod(&m), callbacks(&cbks), snapshot_index(index), has_insert_snapshot_index(false) {}

etcd_module::watcher_callback_list_wrapper_t::~watcher_callback_list_wrapper_t() {
  if (nullptr != mod && 0 != snapshot_index) {
    mod->watcher_snapshot_index_.erase(snapshot_index);
  }
}

void etcd_module::watcher_callback_list_wrapper_t::operator()(const ::atapp::etcd_response_header &header,
                                                              const ::atapp::etcd_watcher::response_t &body) {
  if (nullptr == mod) {
    return;
  }
  mod->last_etcd_event_header_ = header;

  bool enable_snapshot = body.snapshot && 0 != snapshot_index;
  if (enable_snapshot) {
    if (!has_insert_snapshot_index) {
      mod->watcher_snapshot_index_.insert(snapshot_index);
    }

    // We just accept the earliest watcher as snapshot notifier.
    // So just enable by_id or by_name when initializing and they will has the smallest index.
    if (!mod->watcher_snapshot_index_.empty() && *mod->watcher_snapshot_index_.begin() != snapshot_index) {
      enable_snapshot = false;
    }
  }

  etcd_discovery_set::node_by_name_type old_names;
  etcd_discovery_set::node_by_id_type old_ids;
  if (enable_snapshot) {
    _collect_old_nodes(*mod, old_names, old_ids);

    for (auto iter = mod->on_load_snapshot_callbacks_.begin(); iter != mod->on_load_snapshot_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }

  // decode data
  for (size_t i = 0; i < body.events.size(); ++i) {
    const ::atapp::etcd_watcher::event_t &evt_data = body.events[i];
    node_info_t node;
    etcd_discovery_node::node_version current_node_version;

    if (evt_data.kv.value.empty()) {
      unpack(node, evt_data.kv.key, evt_data.prev_kv.value, true);
      current_node_version.create_revision = evt_data.prev_kv.create_revision;
      current_node_version.modify_revision = evt_data.prev_kv.mod_revision;
      current_node_version.version = evt_data.prev_kv.version;
    } else {
      unpack(node, evt_data.kv.key, evt_data.kv.value, true);
      current_node_version.create_revision = evt_data.kv.create_revision;
      current_node_version.modify_revision = evt_data.kv.mod_revision;
      current_node_version.version = evt_data.kv.version;
    }
    if (node.node_discovery.id() == 0 && node.node_discovery.name().empty()) {
      continue;
    }

    if (enable_snapshot) {
      _remove_old_node_index(node.node_discovery, old_names, old_ids);
    }

    if (evt_data.evt_type == ::atapp::etcd_watch_event::EN_WEVT_DELETE) {
      node.action = node_action_t::EN_NAT_DELETE;
    } else {
      node.action = node_action_t::EN_NAT_PUT;
    }

    mod->update_inner_watcher_event(node, current_node_version);

    if (nullptr == callbacks) {
      continue;
    }

    if (callbacks->empty()) {
      continue;
    }

    watcher_sender_list_t sender(*mod, header, body, evt_data, node);
    for (std::list<watcher_list_callback_t>::iterator iter = callbacks->begin(); iter != callbacks->end(); ++iter) {
      if (*iter) {
        (*iter)(std::ref(sender));
      }
    }
  }

  // cleanup old nodes when receive snapshot response
  if (enable_snapshot) {
    etcd_module::watcher_internal_access_t::cleanup_old_nodes(*mod, old_names, old_ids);

    for (auto iter = mod->on_snapshot_loaded_callbacks_.begin(); iter != mod->on_snapshot_loaded_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }
}

etcd_module::watcher_callback_one_wrapper_t::watcher_callback_one_wrapper_t(etcd_module &m, watcher_one_callback_t cbk,
                                                                            int64_t index)
    : mod(&m), callback(cbk), snapshot_index(index), has_insert_snapshot_index(false) {}

etcd_module::watcher_callback_one_wrapper_t::~watcher_callback_one_wrapper_t() {
  if (nullptr != mod && 0 != snapshot_index) {
    mod->watcher_snapshot_index_.erase(snapshot_index);
  }
}

void etcd_module::watcher_callback_one_wrapper_t::operator()(const ::atapp::etcd_response_header &header,
                                                             const ::atapp::etcd_watcher::response_t &body) {
  if (nullptr == mod) {
    return;
  }
  mod->last_etcd_event_header_ = header;

  bool enable_snapshot = body.snapshot && 0 != snapshot_index;
  if (enable_snapshot) {
    if (!has_insert_snapshot_index) {
      mod->watcher_snapshot_index_.insert(snapshot_index);
    }

    // We just accept the earliest watcher as snapshot notifier.
    // So just enable by_id or by_name when initializing and they will has the smallest index.
    if (!mod->watcher_snapshot_index_.empty() && *mod->watcher_snapshot_index_.begin() != snapshot_index) {
      enable_snapshot = false;
    }
  }

  etcd_discovery_set::node_by_name_type old_names;
  etcd_discovery_set::node_by_id_type old_ids;
  if (enable_snapshot) {
    _collect_old_nodes(*mod, old_names, old_ids);

    for (auto iter = mod->on_load_snapshot_callbacks_.begin(); iter != mod->on_load_snapshot_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }

  // decode data
  for (size_t i = 0; i < body.events.size(); ++i) {
    const ::atapp::etcd_watcher::event_t &evt_data = body.events[i];
    node_info_t node;
    etcd_discovery_node::node_version current_node_version;

    if (evt_data.kv.value.empty()) {
      unpack(node, evt_data.kv.key, evt_data.prev_kv.value, true);
      current_node_version.create_revision = evt_data.prev_kv.create_revision;
      current_node_version.modify_revision = evt_data.prev_kv.mod_revision;
      current_node_version.version = evt_data.prev_kv.version;
    } else {
      unpack(node, evt_data.kv.key, evt_data.kv.value, true);
      current_node_version.create_revision = evt_data.kv.create_revision;
      current_node_version.modify_revision = evt_data.kv.mod_revision;
      current_node_version.version = evt_data.kv.version;
    }
    if (node.node_discovery.id() == 0 && node.node_discovery.name().empty()) {
      continue;
    }

    if (enable_snapshot) {
      _remove_old_node_index(node.node_discovery, old_names, old_ids);
    }

    if (evt_data.evt_type == ::atapp::etcd_watch_event::EN_WEVT_DELETE) {
      node.action = node_action_t::EN_NAT_DELETE;
    } else {
      node.action = node_action_t::EN_NAT_PUT;
    }

    mod->update_inner_watcher_event(node, current_node_version);

    if (!callback) {
      continue;
    }
    watcher_sender_one_t sender(*mod, header, body, evt_data, node);
    callback(std::ref(sender));
  }

  // cleanup old nodes when receive snapshot response
  if (enable_snapshot) {
    etcd_module::watcher_internal_access_t::cleanup_old_nodes(*mod, old_names, old_ids);

    for (auto iter = mod->on_snapshot_loaded_callbacks_.begin(); iter != mod->on_snapshot_loaded_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }
}

bool etcd_module::update_inner_watcher_event(node_info_t &node, const etcd_discovery_node::node_version &version) {
  etcd_discovery_node::ptr_t local_cache_by_id = global_discovery_.get_node_by_id(node.node_discovery.id());
  etcd_discovery_node::ptr_t local_cache_by_name = global_discovery_.get_node_by_name(node.node_discovery.name());
  etcd_discovery_node::ptr_t new_inst;

  bool has_event = false;

  UTIL_LIKELY_IF(local_cache_by_id == local_cache_by_name) {
    if (node_action_t::EN_NAT_DELETE == node.action) {
      if (local_cache_by_id) {
        local_cache_by_id->update_version(version, true);
        global_discovery_.remove_node(local_cache_by_id);

        has_event = true;
      }
    } else {
      if (local_cache_by_id && protobuf_equal(local_cache_by_id->get_discovery_info(), node.node_discovery)) {
        return false;
      }

      if (local_cache_by_id) {
        new_inst = local_cache_by_id;
      } else {
        new_inst = std::make_shared<etcd_discovery_node>();
      }

      new_inst->copy_from(node.node_discovery, version);
      global_discovery_.add_node(new_inst);

      has_event = true;
    }
  }
  else {
    if (node_action_t::EN_NAT_DELETE == node.action) {
      if (local_cache_by_id) {
        local_cache_by_id->update_version(version, true);
        global_discovery_.remove_node(local_cache_by_id);
        has_event = true;
      }
      if (local_cache_by_name) {
        local_cache_by_name->update_version(version, true);
        global_discovery_.remove_node(local_cache_by_name);
        has_event = true;
      }
    } else {
      if ((!local_cache_by_id && 0 != node.node_discovery.id()) ||
          (!local_cache_by_name && !node.node_discovery.name().empty())) {
        has_event = true;
      } else if (local_cache_by_id &&
                 false == protobuf_equal(local_cache_by_id->get_discovery_info(), node.node_discovery)) {
        has_event = true;
      } else if (local_cache_by_name &&
                 false == protobuf_equal(local_cache_by_name->get_discovery_info(), node.node_discovery)) {
        has_event = true;
      }

      if (has_event) {
        if (local_cache_by_id) {
          new_inst = local_cache_by_id;
        } else if (local_cache_by_name) {
          new_inst = local_cache_by_name;
        } else {
          new_inst = std::make_shared<etcd_discovery_node>();
        }
        new_inst->copy_from(node.node_discovery, version);

        if (local_cache_by_id && local_cache_by_id != new_inst) {
          local_cache_by_id->update_version(version, false);
          global_discovery_.remove_node(local_cache_by_id);
        }
        if (local_cache_by_name && local_cache_by_name != new_inst) {
          local_cache_by_name->update_version(version, false);
          global_discovery_.remove_node(local_cache_by_name);
        }

        global_discovery_.add_node(new_inst);
      }
    }
  }

  // remove endpoint if got DELETE event
  if (has_event) {
    app *owner = get_app();
    if (node_action_t::EN_NAT_DELETE == node.action && nullptr != owner) {
      if (0 != node.node_discovery.id()) {
        owner->remove_endpoint(node.node_discovery.id());
      }
      if (!node.node_discovery.name().empty()) {
        owner->remove_endpoint(node.node_discovery.name());
      }
    }

    if (nullptr != owner) {
      // notify all connector discovery event
      if (new_inst) {
        owner->trigger_event_on_discovery_event(node.action, new_inst);
        for (node_event_callback_list_t::iterator iter = node_event_callbacks_.begin();
             iter != node_event_callbacks_.end(); ++iter) {
          if (*iter) {
            (*iter)(node.action, new_inst);
          }
        }
      } else if (local_cache_by_name) {
        owner->trigger_event_on_discovery_event(node.action, local_cache_by_name);
        for (node_event_callback_list_t::iterator iter = node_event_callbacks_.begin();
             iter != node_event_callbacks_.end(); ++iter) {
          if (*iter) {
            (*iter)(node.action, local_cache_by_name);
          }
        }
      } else {
        owner->trigger_event_on_discovery_event(node.action, local_cache_by_id);
        for (node_event_callback_list_t::iterator iter = node_event_callbacks_.begin();
             iter != node_event_callbacks_.end(); ++iter) {
          if (*iter) {
            (*iter)(node.action, local_cache_by_id);
          }
        }
      }
    }
  }

  return has_event;
}

void etcd_module::reset_inner_watchers_and_keepalives() {
  if (inner_watcher_by_name_) {
    etcd_ctx_.remove_watcher(inner_watcher_by_name_);
    inner_watcher_by_name_.reset();
  }

  if (inner_watcher_by_id_) {
    etcd_ctx_.remove_watcher(inner_watcher_by_id_);
    inner_watcher_by_id_.reset();
  }

  inner_keepalive_actors_.clear();
}

}  // namespace atapp
