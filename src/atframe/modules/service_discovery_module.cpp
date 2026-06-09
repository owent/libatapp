// Copyright 2026 atframework
// Created by yousongyang

#include "atframe/modules/service_discovery_module.h"

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
#include <atframe/modules/etcd_module.h>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <set>

#if defined(max)
#  undef max
#endif

#if defined(min)
#  undef min
#endif

#define ETCD_MODULE_BY_ID_DIR "by_id"
#define ETCD_MODULE_BY_NAME_DIR "by_name"
#define ETCD_MODULE_TOPOLOGY_DIR "topology"

LIBATAPP_MACRO_NAMESPACE_BEGIN
namespace {

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

  return true;
}

static bool atapp_topology_equal(const atapp::protocol::atapp_topology_info &l,
                                 const atapp::protocol::atapp_topology_info &r) {
  if (l.id() != r.id()) {
    return false;
  }
  if (l.upstream_id() != r.upstream_id()) {
    return false;
  }
  if (l.name() != r.name()) {
    return false;
  }

  if (l.data().label().size() != r.data().label().size()) {
    return false;
  }

  for (const auto &kv : l.data().label()) {
    auto it = r.data().label().find(kv.first);
    if (it == r.data().label().end()) {
      return false;
    }
    if (it->second != kv.second) {
      return false;
    }
  }

  return true;
}

static bool topology_update_version(service_discovery_module::topology_storage_t &storage,
                                    const etcd_data_version &new_version, bool upgrade, uintptr_t context_ptr) {
  bool ret = false;
  if (upgrade) {
    if (new_version.create_revision > storage.version.create_revision) {
      storage.version.create_revision = new_version.create_revision;
      storage.version.version = new_version.version;
      ret = true;
    }

    if (new_version.modify_revision > storage.version.modify_revision) {
      storage.version.modify_revision = new_version.modify_revision;
      storage.version.version = new_version.version;
      ret = true;
    }

    if (new_version.version > storage.version.version) {
      storage.version.version = new_version.version;
      ret = true;
    }
  } else {
    storage.version = new_version;
    ret = true;
  }
  if (ret) {
    storage.context_ptr = context_ptr;
  }

  return ret;
}
}  // namespace

struct ATFW_UTIL_SYMBOL_LOCAL service_discovery_module::topology_watcher_callback_list_wrapper_t {
  service_discovery_module *mod;
  service_discovery_cluster_context *ctx;

  std::recursive_mutex *callback_lock;
  std::list<topology_watcher_list_callback_t> *callbacks;
  int64_t snapshot_index;
  bool has_insert_snapshot_index;

  topology_watcher_callback_list_wrapper_t(service_discovery_module &m, service_discovery_cluster_context &c,
                                           int64_t index);
  topology_watcher_callback_list_wrapper_t(service_discovery_module &m, service_discovery_cluster_context &c,
                                           std::recursive_mutex &lock,
                                           std::list<topology_watcher_list_callback_t> &cbks, int64_t index);
  ~topology_watcher_callback_list_wrapper_t();
  void operator()(const ::atframework::atapp::etcd_response_header &header,
                  const ::atframework::atapp::etcd_watcher::response_t &body);
};

struct ATFW_UTIL_SYMBOL_LOCAL service_discovery_module::discovery_watcher_callback_list_wrapper_t {
  service_discovery_module *mod;
  service_discovery_cluster_context *ctx;

  std::recursive_mutex *callback_lock;
  std::list<discovery_watcher_list_callback_t> *callbacks;
  int64_t snapshot_index;
  bool has_insert_snapshot_index;

  discovery_watcher_callback_list_wrapper_t(service_discovery_module &m, service_discovery_cluster_context &c,
                                            int64_t index);
  discovery_watcher_callback_list_wrapper_t(service_discovery_module &m, service_discovery_cluster_context &c,
                                            std::recursive_mutex &lock,
                                            std::list<discovery_watcher_list_callback_t> &cbks, int64_t index);
  ~discovery_watcher_callback_list_wrapper_t();
  void operator()(const ::atframework::atapp::etcd_response_header &header,
                  const ::atframework::atapp::etcd_watcher::response_t &body);
};

struct ATFW_UTIL_SYMBOL_LOCAL service_discovery_module::watcher_internal_access_t {
  static void cleanup_old_nodes(service_discovery_module &mod, service_discovery_cluster_context &c,
                                etcd_discovery_set::node_by_name_type &old_names,
                                etcd_discovery_set::node_by_id_type &old_ids);
  static void cleanup_old_topology_peers(service_discovery_module &mod, service_discovery_cluster_context &c,
                                         std::unordered_map<uint64_t, topology_storage_t> &old_ids);
};

struct ATFW_UTIL_SYMBOL_LOCAL service_discovery_module::service_discovery_cluster_context_data {
  service_discovery_cluster_context_data()
      : last_etcd_event_topology_header_{},
        last_etcd_event_discovery_header_{},
        discovery_watcher_snapshot_index_allocator_(0),
        topology_watcher_snapshot_index_allocator_(0) {
    last_etcd_event_topology_header_.cluster_id = 0;
    last_etcd_event_topology_header_.member_id = 0;
    last_etcd_event_topology_header_.raft_term = 0;
    last_etcd_event_topology_header_.revision = 0;

    last_etcd_event_discovery_header_.cluster_id = 0;
    last_etcd_event_discovery_header_.member_id = 0;
    last_etcd_event_discovery_header_.raft_term = 0;
    last_etcd_event_discovery_header_.revision = 0;
  }

  ::atframework::atapp::etcd_response_header last_etcd_event_topology_header_;
  ::atframework::atapp::etcd_response_header last_etcd_event_discovery_header_;

  std::set<int64_t> discovery_watcher_snapshot_index_;
  int64_t discovery_watcher_snapshot_index_allocator_;
  std::set<int64_t> topology_watcher_snapshot_index_;
  int64_t topology_watcher_snapshot_index_allocator_;
  etcd_watcher::ptr_t internal_topology_watcher_;
  etcd_watcher::ptr_t internal_discovery_watcher_by_name_;
  etcd_watcher::ptr_t internal_discovery_watcher_by_id_;

  std::list<etcd_keepalive::ptr_t> internal_topology_keepalive_actors_;
  std::list<etcd_keepalive::ptr_t> internal_discovery_keepalive_actors_;

  std::list<discovery_watcher_list_callback_t> discovery_watcher_by_id_callbacks_;
  std::list<discovery_watcher_list_callback_t> discovery_watcher_by_name_callbacks_;
  std::list<topology_watcher_list_callback_t> topology_watcher_callbacks_;

  mutable std::recursive_mutex discovery_watcher_callback_lock_;
  mutable std::recursive_mutex topology_watcher_callback_lock_;
};

LIBATAPP_MACRO_API
service_discovery_module::service_discovery_module()
    : discovery_enabled_(true),
      maybe_update_internal_keepalive_topology_value_(true),
      maybe_update_internal_keepalive_discovery_value_(true),
      maybe_update_internal_keepalive_discovery_area_(false),
      maybe_update_internal_keepalive_discovery_metadata_(false) {}

LIBATAPP_MACRO_API int service_discovery_module::init() {
  int ret = cluster_context_.init(*get_app(), get_app()->get_origin_configure().etcd(), nullptr);
  if (ret < 0) {
    return ret;
  }

  if (!is_discovery_enabled()) {
    return 0;
  }

  ret = init_service_discovery_keepalives_watchers(cluster_context_);
  if (ret < 0) {
    stop();
    return ret;
  }

  if (!cluster_context_.check_keepalive_actor_start_success()) {
    stop();
    return -1;
  }

  return 0;
}

LIBATAPP_MACRO_API const char *ATFW_UTIL_MACRO_NONNULL service_discovery_module::name() const {
  return "atapp: service_discovery module";
}

LIBATAPP_MACRO_API service_discovery_module::service_discovery_cluster_context::service_discovery_cluster_context()
    : data_(std::make_shared<service_discovery_cluster_context_data>()) {}

LIBATAPP_MACRO_API int service_discovery_module::stop() { return cluster_context_.stop(); }

LIBATAPP_MACRO_API int service_discovery_module::reload() {
  return cluster_context_.reload(get_app()->get_origin_configure().etcd(), nullptr);
}

LIBATAPP_MACRO_API int service_discovery_module::tick() {
  cluster_context_.tick();

  update_keepalive_topology_value();
  if (cluster_context_.get_etcd_module().get_etcd_cluster().check_flag(etcd_cluster::flag_t::kRunning)) {
    update_keepalive_discovery_value();
  }

  return 0;
}

LIBATAPP_MACRO_API bool service_discovery_module::is_discovery_enabled() const {
  return cluster_context_.get_etcd_module().is_etcd_enabled() && discovery_enabled_;
}

LIBATAPP_MACRO_API void service_discovery_module::enable_discovery() {
  if (is_discovery_enabled()) {
    return;
  }
  discovery_enabled_ = true;
  if (!is_discovery_enabled()) {
    return;
  }
  init_service_discovery_keepalives_watchers(cluster_context_);
}

LIBATAPP_MACRO_API void service_discovery_module::disable_discovery() {
  if (!is_discovery_enabled()) {
    return;
  }
  discovery_enabled_ = false;
  cluster_context_.reset_internal_watchers_and_keepalives();
}

void service_discovery_module::update_keepalive_topology_value() {
  if (!maybe_update_internal_keepalive_topology_value_ || nullptr == get_app()) {
    return;
  }
  maybe_update_internal_keepalive_topology_value_ = false;

  atapp_topology_info_ptr_t topology_ptr = atfw::util::memory::make_strong_rc<atapp::protocol::atapp_topology_info>();
  get_app()->pack(*topology_ptr);

  if (last_submitted_topology_data_ && atapp_topology_equal(*topology_ptr, *last_submitted_topology_data_)) {
    return;
  }
  last_submitted_topology_data_ = topology_ptr;

  std::string new_value;
  pack(*topology_ptr, new_value);

  if (new_value != internal_keepalive_topology_value_) {
    internal_keepalive_topology_value_.swap(new_value);

    for (std::list<etcd_keepalive::ptr_t>::iterator iter =
             cluster_context_.data_->internal_topology_keepalive_actors_.begin();
         iter != cluster_context_.data_->internal_topology_keepalive_actors_.end(); ++iter) {
      if (*iter) {
        (*iter)->set_value(internal_keepalive_topology_value_);
      }
    }
  }
}

void service_discovery_module::update_keepalive_discovery_value() {
  if (!(maybe_update_internal_keepalive_discovery_value_ || maybe_update_internal_keepalive_discovery_area_ ||
        maybe_update_internal_keepalive_discovery_metadata_) ||
      nullptr == get_app()) {
    return;
  }

  std::string new_value;
  node_info_t ni;
  get_app()->pack(ni.node_discovery);

  bool has_changed = maybe_update_internal_keepalive_discovery_value_;
  maybe_update_internal_keepalive_discovery_value_ = false;
  if (maybe_update_internal_keepalive_discovery_area_) {
    if (!atapp_discovery_equal(ni.node_discovery.area(), last_submitted_discovery_data_area_)) {
      has_changed = true;
      last_submitted_discovery_data_area_ = ni.node_discovery.area();
    }
  }
  maybe_update_internal_keepalive_discovery_area_ = false;
  if (maybe_update_internal_keepalive_discovery_metadata_) {
    if (!etcd_discovery_set::metadata_equal_type()(ni.node_discovery.metadata(),
                                                   last_submitted_discovery_data_metadata_)) {
      has_changed = true;
      last_submitted_discovery_data_metadata_ = ni.node_discovery.metadata();
    }
  }
  maybe_update_internal_keepalive_discovery_metadata_ = false;

  if (!has_changed) {
    return;
  }

  pack(ni, new_value);
  if (new_value != internal_keepalive_discovery_value_) {
    internal_keepalive_discovery_value_.swap(new_value);

    for (std::list<etcd_keepalive::ptr_t>::iterator iter =
             cluster_context_.data_->internal_discovery_keepalive_actors_.begin();
         iter != cluster_context_.data_->internal_discovery_keepalive_actors_.end(); ++iter) {
      if (*iter) {
        (*iter)->set_value(internal_keepalive_discovery_value_);
      }
    }
  }
}

LIBATAPP_MACRO_API int service_discovery_module::init_service_discovery_keepalives_watchers(
    service_discovery_cluster_context &context) {
  // 先刷新topology数据，后刷新discovery数据，以保证策略路由变化时已获取到最新的topology信息
  update_keepalive_topology_value();
  update_keepalive_discovery_value();

  atframework::atapp::app &app = *get_app();

  int ret = init_topology_keepalive(app, context, internal_keepalive_topology_value_);
  if (ret < 0) {
    return ret;
  }
  ret = init_discovery_keepalive_by_id(app, context, internal_keepalive_discovery_value_);
  if (ret < 0) {
    return ret;
  }
  ret = init_discovery_keepalive_by_name(app, context, internal_keepalive_discovery_value_);
  if (ret < 0) {
    return ret;
  }

  ret = add_topology_watcher_callback(nullptr);
  if (ret < 0) {
    return ret;
  }
  ret = add_discovery_watcher_by_id_callback(nullptr);
  if (ret < 0) {
    return ret;
  }
  ret = add_discovery_watcher_by_name_callback(nullptr);
  if (ret < 0) {
    return ret;
  }

  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::init_discovery_keepalive_by_id(
    atframework::atapp::app &app, service_discovery_cluster_context &context, std::string &value) {
  atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(
      app, context, value, get_discovery_by_id_path(app, context.etcd_module_.get_configure_path()));
  if (!actor) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(),
                                          "create etcd_keepalive for by_id index failed.");
    return -1;
  }

  context.data_->internal_discovery_keepalive_actors_.push_back(actor);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(context.etcd_module_.get_etcd_cluster(),
                                       "create etcd_keepalive {} for by_id index {} success",
                                       reinterpret_cast<const void *>(actor.get()),
                                       get_discovery_by_id_path(app, context.etcd_module_.get_configure_path()));
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::init_discovery_keepalive_by_name(
    atframework::atapp::app &app, service_discovery_cluster_context &context, std::string &value) {
  atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(
      app, context, value, get_discovery_by_name_path(app, context.etcd_module_.get_configure_path()));
  if (!actor) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(),
                                          "create etcd_keepalive for by_name index failed.");
    return -1;
  }

  context.data_->internal_discovery_keepalive_actors_.push_back(actor);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(context.etcd_module_.get_etcd_cluster(),
                                       "create etcd_keepalive {} for by_name index {} success",
                                       reinterpret_cast<const void *>(actor.get()),
                                       get_discovery_by_name_path(app, context.etcd_module_.get_configure_path()));
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::init_topology_keepalive(atframework::atapp::app &app,
                                                                         service_discovery_cluster_context &context,
                                                                         std::string &value) {
  atapp::etcd_keepalive::ptr_t actor =
      add_keepalive_actor(app, context, value, get_topology_path(app, context.etcd_module_.get_configure_path()));
  if (!actor) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(),
                                          "create etcd_keepalive for topology index failed.");
    return -1;
  }

  context.data_->internal_topology_keepalive_actors_.push_back(actor);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(
      context.etcd_module_.get_etcd_cluster(), "create etcd_keepalive {} for topology index {} success",
      reinterpret_cast<const void *>(actor.get()), get_topology_path(app, context.etcd_module_.get_configure_path()));
  return 0;
}

LIBATAPP_MACRO_API void service_discovery_module::set_maybe_update_keepalive_topology_value() {
  maybe_update_internal_keepalive_topology_value_ = true;
}

LIBATAPP_MACRO_API void service_discovery_module::set_maybe_update_keepalive_discovery_value() {
  maybe_update_internal_keepalive_discovery_value_ = true;
}

LIBATAPP_MACRO_API void service_discovery_module::set_maybe_update_keepalive_discovery_area() {
  maybe_update_internal_keepalive_discovery_area_ = true;
}

LIBATAPP_MACRO_API void service_discovery_module::set_maybe_update_keepalive_discovery_metadata() {
  maybe_update_internal_keepalive_discovery_metadata_ = true;
}

LIBATAPP_MACRO_API std::string service_discovery_module::get_discovery_by_id_path(atframework::atapp::app &app,
                                                                                  const std::string &path) {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}-{}", path, ETCD_MODULE_BY_ID_DIR, app.get_app_name(), app.get_id());
}

LIBATAPP_MACRO_API std::string service_discovery_module::get_discovery_by_name_path(atframework::atapp::app &app,
                                                                                    const std::string &path) {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}-{}", path, ETCD_MODULE_BY_NAME_DIR, app.get_app_name(), app.get_id());
}

LIBATAPP_MACRO_API std::string service_discovery_module::get_topology_path(atframework::atapp::app &app,
                                                                           const std::string &path) {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}/{}-{}", path, ETCD_MODULE_TOPOLOGY_DIR, app.get_app_name(), app.get_id());
}

LIBATAPP_MACRO_API std::string service_discovery_module::get_discovery_by_id_watcher_path(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, const std::string &path) {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}", path, ETCD_MODULE_BY_ID_DIR);
}

LIBATAPP_MACRO_API std::string service_discovery_module::get_discovery_by_name_watcher_path(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, const std::string &path) {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}", path, ETCD_MODULE_BY_NAME_DIR);
}

LIBATAPP_MACRO_API std::string service_discovery_module::get_topology_watcher_path(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, const std::string &path) {
  return LOG_WRAPPER_FWAPI_FORMAT("{}{}", path, ETCD_MODULE_TOPOLOGY_DIR);
}

LIBATAPP_MACRO_API int service_discovery_module::init_discovery_watcher_by_id(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, service_discovery_cluster_context &context) {
  if (!context.data_->internal_discovery_watcher_by_id_) {
    std::string watch_path =
        LOG_WRAPPER_FWAPI_FORMAT("{}{}", context.etcd_module_.get_configure_path(), ETCD_MODULE_BY_ID_DIR);

    context.data_->internal_discovery_watcher_by_id_ =
        atapp::etcd_watcher::create(context.etcd_module_.get_etcd_cluster(), watch_path, "+1");
    if (!context.data_->internal_discovery_watcher_by_id_) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(),
                                            "create etcd_watcher by_id failed.");
      return EN_ATBUS_ERR_MALLOC;
    }

    context.data_->internal_discovery_watcher_by_id_->set_conf_from_protobuf(
        context.etcd_module_.get_configure().watcher());
    context.etcd_module_.get_etcd_cluster().add_watcher(context.data_->internal_discovery_watcher_by_id_);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(context.etcd_module_.get_etcd_cluster(),
                                         "create etcd_watcher for by_id index {} success", watch_path);

    context.data_->internal_discovery_watcher_by_id_->set_evt_handle(discovery_watcher_callback_list_wrapper_t(
        *app.get_service_discovery_module(), context, context.data_->discovery_watcher_callback_lock_,
        context.data_->discovery_watcher_by_id_callbacks_,
        ++context.data_->discovery_watcher_snapshot_index_allocator_));
  }
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::init_discovery_watcher_by_name(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, service_discovery_cluster_context &context) {
  if (!context.data_->internal_discovery_watcher_by_name_) {
    // generate watch data
    std::string watch_path =
        LOG_WRAPPER_FWAPI_FORMAT("{}{}", context.etcd_module_.get_configure_path(), ETCD_MODULE_BY_NAME_DIR);

    context.data_->internal_discovery_watcher_by_name_ =
        atapp::etcd_watcher::create(context.etcd_module_.get_etcd_cluster(), watch_path, "+1");
    if (!context.data_->internal_discovery_watcher_by_name_) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(),
                                            "create etcd_watcher by_name failed.");
      return EN_ATBUS_ERR_MALLOC;
    }

    context.data_->internal_discovery_watcher_by_name_->set_conf_from_protobuf(
        context.etcd_module_.get_configure().watcher());
    context.etcd_module_.get_etcd_cluster().add_watcher(context.data_->internal_discovery_watcher_by_name_);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(context.etcd_module_.get_etcd_cluster(),
                                         "create etcd_watcher for by_name index {} success", watch_path);

    context.data_->internal_discovery_watcher_by_name_->set_evt_handle(discovery_watcher_callback_list_wrapper_t(
        *app.get_service_discovery_module(), context, context.data_->discovery_watcher_callback_lock_,
        context.data_->discovery_watcher_by_name_callbacks_,
        ++context.data_->discovery_watcher_snapshot_index_allocator_));
  }
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::init_topology_watcher(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, service_discovery_cluster_context &context) {
  if (!context.data_->internal_topology_watcher_) {
    // generate watch data
    std::string watch_path =
        LOG_WRAPPER_FWAPI_FORMAT("{}{}", context.etcd_module_.get_configure_path(), ETCD_MODULE_TOPOLOGY_DIR);

    context.data_->internal_topology_watcher_ =
        atapp::etcd_watcher::create(context.etcd_module_.get_etcd_cluster(), watch_path, "+1");
    if (!context.data_->internal_topology_watcher_) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(),
                                            "create etcd_watcher topology failed.");
      return EN_ATBUS_ERR_MALLOC;
    }

    context.data_->internal_topology_watcher_->set_conf_from_protobuf(context.etcd_module_.get_configure().watcher());
    context.etcd_module_.get_etcd_cluster().add_watcher(context.data_->internal_topology_watcher_);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_INFO(context.etcd_module_.get_etcd_cluster(),
                                         "create etcd_watcher for topology index {} success", watch_path);

    context.data_->internal_topology_watcher_->set_evt_handle(topology_watcher_callback_list_wrapper_t(
        *app.get_service_discovery_module(), context, context.data_->topology_watcher_callback_lock_,
        context.data_->topology_watcher_callbacks_, ++context.data_->topology_watcher_snapshot_index_allocator_));
  }
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::service_discovery_cluster_context::init(
    atframework::atapp::app &app, const atapp::protocol::atapp_etcd &conf,
    const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf) {
  return etcd_module_.init(app, conf, log_conf);
}

LIBATAPP_MACRO_API int service_discovery_module::service_discovery_cluster_context::reload(
    const atapp::protocol::atapp_etcd &conf, const atapp::protocol::atapp_log *ATFW_UTIL_MACRO_NULLABLE log_conf) {
  return etcd_module_.reload(conf, log_conf);
}

LIBATAPP_MACRO_API int service_discovery_module::service_discovery_cluster_context::stop() {
  reset_internal_watchers_and_keepalives();
  return etcd_module_.stop();
}

LIBATAPP_MACRO_API void service_discovery_module::service_discovery_cluster_context::tick() { etcd_module_.tick(); }

LIBATAPP_MACRO_API etcd_module &service_discovery_module::service_discovery_cluster_context::get_etcd_module() {
  return etcd_module_;
}

LIBATAPP_MACRO_API const etcd_module &service_discovery_module::service_discovery_cluster_context::get_etcd_module()
    const {
  return etcd_module_;
}

LIBATAPP_MACRO_API bool
service_discovery_module::service_discovery_cluster_context::check_keepalive_actor_start_success() {
  const std::list<etcd_keepalive::ptr_t> *keepalive_actors[] = {&data_->internal_discovery_keepalive_actors_,
                                                                &data_->internal_topology_keepalive_actors_};

  return etcd_module_.check_keepalive_actor_start_success(gsl::make_span(keepalive_actors));
}

LIBATAPP_MACRO_API void
service_discovery_module::service_discovery_cluster_context::add_discovery_watcher_by_id_callback(
    const discovery_watcher_list_callback_t &fn) {
  if (fn) {
    std::lock_guard<std::recursive_mutex> lock_guard{data_->discovery_watcher_callback_lock_};
    data_->discovery_watcher_by_id_callbacks_.push_back(fn);
  }
}

LIBATAPP_MACRO_API void
service_discovery_module::service_discovery_cluster_context::add_discovery_watcher_by_name_callback(
    const discovery_watcher_list_callback_t &fn) {
  if (fn) {
    std::lock_guard<std::recursive_mutex> lock_guard{data_->discovery_watcher_callback_lock_};
    data_->discovery_watcher_by_name_callbacks_.push_back(fn);
  }
}

LIBATAPP_MACRO_API void service_discovery_module::service_discovery_cluster_context::add_topology_watcher_callback(
    const topology_watcher_list_callback_t &fn) {
  if (fn) {
    std::lock_guard<std::recursive_mutex> lock_guard{data_->topology_watcher_callback_lock_};
    data_->topology_watcher_callbacks_.push_back(fn);
  }
}

LIBATAPP_MACRO_API int service_discovery_module::add_discovery_watcher_by_id_callback(
    const discovery_watcher_list_callback_t &fn) {
  int ret = init_discovery_watcher_by_id(*get_app(), cluster_context_);
  if (ret < 0) {
    return ret;
  }
  cluster_context_.add_discovery_watcher_by_id_callback(fn);
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::add_discovery_watcher_by_name_callback(
    const discovery_watcher_list_callback_t &fn) {
  int ret = init_discovery_watcher_by_name(*get_app(), cluster_context_);
  if (ret < 0) {
    return ret;
  }
  cluster_context_.add_discovery_watcher_by_name_callback(fn);
  return 0;
}

LIBATAPP_MACRO_API int service_discovery_module::add_topology_watcher_callback(
    const topology_watcher_list_callback_t &fn) {
  int ret = init_topology_watcher(*get_app(), cluster_context_);
  if (ret < 0) {
    return ret;
  }
  cluster_context_.add_topology_watcher_callback(fn);
  return 0;
}

LIBATAPP_MACRO_API const ::atframework::atapp::etcd_cluster &service_discovery_module::get_raw_etcd_ctx() const {
  return cluster_context_.get_etcd_module().get_etcd_cluster();
}
LIBATAPP_MACRO_API ::atframework::atapp::etcd_cluster &service_discovery_module::get_raw_etcd_ctx() {
  return cluster_context_.get_etcd_module().get_etcd_cluster();
}

LIBATAPP_MACRO_API const ::atframework::atapp::etcd_response_header &
service_discovery_module::get_last_etcd_event_topology_header() const noexcept {
  return cluster_context_.data_->last_etcd_event_topology_header_;
}

LIBATAPP_MACRO_API const ::atframework::atapp::etcd_response_header &
service_discovery_module::get_last_etcd_event_discovery_header() const noexcept {
  return cluster_context_.data_->last_etcd_event_discovery_header_;
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &service_discovery_module::get_configure() const {
  return get_app()->get_origin_configure().etcd();
}

LIBATAPP_MACRO_API const std::string &service_discovery_module::get_configure_path() {
  return cluster_context_.get_etcd_module().get_configure_path();
}

LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t service_discovery_module::add_keepalive_actor(
    atframework::atapp::app &app, service_discovery_cluster_context &context, std::string &val,
    const std::string &node_path) {
  atapp::etcd_keepalive::ptr_t ret;

  if (val.empty()) {
    node_info_t ni;
    app.pack(ni.node_discovery);
    pack(ni, val);
  }

  ret = atapp::etcd_keepalive::create(context.etcd_module_.get_etcd_cluster(), node_path);
  if (!ret) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(context.etcd_module_.get_etcd_cluster(), "create etcd_keepalive failed.");
    return ret;
  }

  ret->set_checker(val);
  ret->set_value(val);

  if (!context.etcd_module_.get_etcd_cluster().add_keepalive(ret)) {
    ret.reset();
  }

  return ret;
}

LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t service_discovery_module::add_keepalive_actor(
    atframework::atapp::app &app, std::string &val, const std::string &node_path) {
  return add_keepalive_actor(app, cluster_context_, val, node_path);
}

LIBATAPP_MACRO_API bool service_discovery_module::remove_keepalive_actor(
    ATFW_EXPLICIT_UNUSED_ATTR atframework::atapp::app &app, service_discovery_cluster_context &context,
    const atapp::etcd_keepalive::ptr_t &keepalive) {
  if (!keepalive) {
    return false;
  }

  return context.etcd_module_.get_etcd_cluster().remove_keepalive(keepalive);
}

LIBATAPP_MACRO_API bool service_discovery_module::remove_keepalive_actor(
    atframework::atapp::app &app, const atapp::etcd_keepalive::ptr_t &keepalive) {
  std::list<etcd_keepalive::ptr_t> *keepalive_actors[] = {&cluster_context_.data_->internal_discovery_keepalive_actors_,
                                                          &cluster_context_.data_->internal_topology_keepalive_actors_};

  for (auto *keepalive_actor_list : keepalive_actors) {
    std::list<etcd_keepalive::ptr_t>::iterator internal_iter =
        std::find(keepalive_actor_list->begin(), keepalive_actor_list->end(), keepalive);
    if (internal_iter != keepalive_actor_list->end()) {
      keepalive_actor_list->erase(internal_iter);
    }
  }
  return remove_keepalive_actor(app, cluster_context_, keepalive);
}

LIBATAPP_MACRO_API service_discovery_module::node_event_callback_handle_t
service_discovery_module::add_on_node_discovery_event(const node_event_callback_t &fn) {
  std::lock_guard<std::recursive_mutex> lock_guard{node_event_lock_};
  if (!fn) {
    return node_event_callbacks_.end();
  }

  return node_event_callbacks_.insert(node_event_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void service_discovery_module::remove_on_node_event(node_event_callback_handle_t &handle) {
  std::lock_guard<std::recursive_mutex> lock_guard{node_event_lock_};
  if (handle == node_event_callbacks_.end()) {
    return;
  }

  node_event_callbacks_.erase(handle);
  handle = node_event_callbacks_.end();
}

LIBATAPP_MACRO_API service_discovery_module::topology_info_event_callback_handle_t
service_discovery_module::add_on_topology_info_event(const topology_info_event_callback_t &fn) {
  std::lock_guard<std::recursive_mutex> lock_guard{topology_info_event_lock_};
  if (!fn) {
    return topology_info_event_callbacks_.end();
  }

  return topology_info_event_callbacks_.insert(topology_info_event_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void service_discovery_module::remove_on_topology_info_event(
    topology_info_event_callback_handle_t &handle) {
  std::lock_guard<std::recursive_mutex> lock_guard{topology_info_event_lock_};
  if (handle == topology_info_event_callbacks_.end()) {
    return;
  }

  topology_info_event_callbacks_.erase(handle);
  handle = topology_info_event_callbacks_.end();
}

LIBATAPP_MACRO_API etcd_discovery_set &service_discovery_module::get_global_discovery() noexcept {
  return global_discovery_;
}

LIBATAPP_MACRO_API const etcd_discovery_set &service_discovery_module::get_global_discovery() const noexcept {
  return global_discovery_;
}

LIBATAPP_MACRO_API const std::unordered_map<uint64_t, service_discovery_module::topology_storage_t> &
service_discovery_module::get_topology_info_set() const noexcept {
  return internal_topology_info_set_;
}

LIBATAPP_MACRO_API bool service_discovery_module::has_discovery_snapshot() const noexcept {
  return !cluster_context_.data_->discovery_watcher_snapshot_index_.empty();
}

LIBATAPP_MACRO_API service_discovery_module::discovery_snapshot_event_callback_handle_t
service_discovery_module::add_on_load_discovery_snapshot(const discovery_snapshot_event_callback_t &fn) {
  if (!fn) {
    return discovery_on_load_snapshot_callbacks_.end();
  }

  return discovery_on_load_snapshot_callbacks_.insert(discovery_on_load_snapshot_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void service_discovery_module::remove_on_load_discovery_snapshot(
    discovery_snapshot_event_callback_handle_t &handle) {
  if (handle == discovery_on_load_snapshot_callbacks_.end()) {
    return;
  }

  discovery_on_load_snapshot_callbacks_.erase(handle);
  handle = discovery_on_load_snapshot_callbacks_.end();
}

LIBATAPP_MACRO_API service_discovery_module::discovery_snapshot_event_callback_handle_t
service_discovery_module::add_on_discovery_snapshot_loaded(const discovery_snapshot_event_callback_t &fn) {
  if (!fn) {
    return discovery_on_snapshot_loaded_callbacks_.end();
  }

  return discovery_on_snapshot_loaded_callbacks_.insert(discovery_on_snapshot_loaded_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void service_discovery_module::remove_on_discovery_snapshot_loaded(
    discovery_snapshot_event_callback_handle_t &handle) {
  if (handle == discovery_on_snapshot_loaded_callbacks_.end()) {
    return;
  }

  discovery_on_snapshot_loaded_callbacks_.erase(handle);
  handle = discovery_on_snapshot_loaded_callbacks_.end();
}

LIBATAPP_MACRO_API bool service_discovery_module::has_topology_snapshot() const noexcept {
  return !cluster_context_.data_->topology_watcher_snapshot_index_.empty();
}

LIBATAPP_MACRO_API service_discovery_module::topology_snapshot_event_callback_handle_t
service_discovery_module::add_on_load_topology_snapshot(const topology_snapshot_event_callback_t &fn) {
  if (!fn) {
    return topology_on_load_snapshot_callbacks_.end();
  }

  return topology_on_load_snapshot_callbacks_.insert(topology_on_load_snapshot_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void service_discovery_module::remove_on_load_topology_snapshot(
    topology_snapshot_event_callback_handle_t &handle) {
  if (handle == topology_on_load_snapshot_callbacks_.end()) {
    return;
  }

  topology_on_load_snapshot_callbacks_.erase(handle);
  handle = topology_on_load_snapshot_callbacks_.end();
}

LIBATAPP_MACRO_API service_discovery_module::topology_snapshot_event_callback_handle_t
service_discovery_module::add_on_topology_snapshot_loaded(const topology_snapshot_event_callback_t &fn) {
  if (!fn) {
    return topology_on_snapshot_loaded_callbacks_.end();
  }

  return topology_on_snapshot_loaded_callbacks_.insert(topology_on_snapshot_loaded_callbacks_.end(), fn);
}

LIBATAPP_MACRO_API void service_discovery_module::remove_on_topology_snapshot_loaded(
    topology_snapshot_event_callback_handle_t &handle) {
  if (handle == topology_on_snapshot_loaded_callbacks_.end()) {
    return;
  }

  topology_on_snapshot_loaded_callbacks_.erase(handle);
  handle = topology_on_snapshot_loaded_callbacks_.end();
}

LIBATAPP_MACRO_API etcd_watcher::watch_event_fn_t_ptr
service_discovery_module::create_discovery_watcher_callback_list_wrapper(service_discovery_cluster_context &context) {
  return std::make_unique<etcd_watcher::watch_event_fn_t>(discovery_watcher_callback_list_wrapper_t(
      *this, context, ++context.data_->discovery_watcher_snapshot_index_allocator_));
}
LIBATAPP_MACRO_API etcd_watcher::watch_event_fn_t_ptr
service_discovery_module::create_topology_watcher_callback_list_wrapper(service_discovery_cluster_context &context) {
  return std::make_unique<etcd_watcher::watch_event_fn_t>(topology_watcher_callback_list_wrapper_t(
      *this, context, ++context.data_->topology_watcher_snapshot_index_allocator_));
}

bool service_discovery_module::unpack(node_info_t &out, const std::string &path, const std::string &json,
                                      bool reset_data) {
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
        out.node_discovery.set_id(atfw::util::string::to_int<uint64_t>(path.c_str() + last_minus + 1));
        out.node_discovery.set_name(path.substr(start_idx, last_minus - start_idx));
      } else {
        // old mode, only id on last segment
        out.node_discovery.set_id(atfw::util::string::to_int<uint64_t>(&path[start_idx]));
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

void service_discovery_module::pack(const node_info_t &src, std::string &json) {
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonPrintOptions options;
  options.add_whitespace = false;
  options.always_print_enums_as_ints = true;
  options.preserve_proto_field_names = true;
  options.unquote_int64_if_possible = true;
  if (!ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::MessageToJsonString(src.node_discovery, &json, options).ok()) {
    FWLOGERROR("service_discovery_module pack message to json failed");
  }
}

bool service_discovery_module::unpack(topology_info_t &out, const std::string &path, const std::string &json,
                                      bool reset_data) {
  if (reset_data && out.storage.info) {
    out.storage.info->Clear();
  }
  if (!out.storage.info) {
    out.storage.info = atfw::util::memory::make_strong_rc<atapp::protocol::atapp_topology_info>();
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
        out.storage.info->set_id(atfw::util::string::to_int<uint64_t>(path.c_str() + last_minus + 1));
        out.storage.info->set_name(path.substr(start_idx, last_minus - start_idx));
      } else {
        // old mode, only id on last segment
        out.storage.info->set_id(atfw::util::string::to_int<uint64_t>(&path[start_idx]));
      }
    }
    return false;
  }

  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;

  if (!ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonStringToMessage(json, out.storage.info.get(), options).ok()) {
    return false;
  }

  return true;
}

void service_discovery_module::pack(const atapp::protocol::atapp_topology_info &src, std::string &json) {
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::JsonPrintOptions options;
  options.add_whitespace = false;
  options.always_print_enums_as_ints = true;
  options.preserve_proto_field_names = true;
  options.unquote_int64_if_possible = true;
  if (!ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::util::MessageToJsonString(src, &json, options).ok()) {
    FWLOGERROR("service_discovery_module pack message to json failed");
  }
}

namespace {
static void _collect_old_nodes(service_discovery_module &mod, etcd_discovery_set::node_by_name_type &old_names,
                               etcd_discovery_set::node_by_id_type &old_ids) {
  old_ids.reserve(mod.get_global_discovery().get_sorted_nodes().size());
  old_names.reserve(mod.get_global_discovery().get_sorted_nodes().size());
  for (const auto &node : mod.get_global_discovery().get_sorted_nodes()) {
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
}  // namespace

void service_discovery_module::watcher_internal_access_t::cleanup_old_nodes(
    service_discovery_module &mod, service_discovery_cluster_context &ctx,
    etcd_discovery_set::node_by_name_type &old_names, etcd_discovery_set::node_by_id_type &old_ids) {
  for (auto &node : old_names) {
    if (node.second->get_context_ptr() != reinterpret_cast<uintptr_t>(&ctx)) {
      continue;
    }
    service_discovery_module::node_info_t evt_node;
    evt_node.action = service_discovery_module::node_action_t::kDelete;
    evt_node.context_ptr = reinterpret_cast<uintptr_t>(&ctx);
    node.second->copy_key_to(evt_node.node_discovery);
    mod.update_internal_watcher_event(evt_node, node.second->get_version());
  }

  for (auto &node : old_ids) {
    if (node.second->get_context_ptr() != reinterpret_cast<uintptr_t>(&ctx)) {
      continue;
    }
    service_discovery_module::node_info_t evt_node;
    evt_node.action = service_discovery_module::node_action_t::kDelete;
    evt_node.context_ptr = reinterpret_cast<uintptr_t>(&ctx);
    node.second->copy_key_to(evt_node.node_discovery);
    mod.update_internal_watcher_event(evt_node, node.second->get_version());
  }
}

namespace {
static void _collect_old_topology_peers(
    service_discovery_module &mod,
    std::unordered_map<uint64_t, service_discovery_module::topology_storage_t> &old_ids) {
  old_ids.reserve(mod.get_topology_info_set().size());
  old_ids = mod.get_topology_info_set();
}

static void _remove_old_topology_peer_index(
    const atapp::protocol::atapp_topology_info &topology_info,
    std::unordered_map<uint64_t, service_discovery_module::topology_storage_t> &old_ids) {
  if (0 != topology_info.id()) {
    old_ids.erase(topology_info.id());
  }
}
}  // namespace

void service_discovery_module::watcher_internal_access_t::cleanup_old_topology_peers(
    service_discovery_module &mod, service_discovery_cluster_context &ctx,
    std::unordered_map<uint64_t, service_discovery_module::topology_storage_t> &old_ids) {
  for (auto &id : old_ids) {
    if (id.second.context_ptr != reinterpret_cast<uintptr_t>(&ctx)) {
      continue;
    }
    service_discovery_module::topology_info_t evt_node;
    evt_node.action = service_discovery_module::topology_action_t::kDelete;
    evt_node.storage = id.second;
    mod.update_internal_watcher_event(evt_node);
  }
}

service_discovery_module::discovery_watcher_callback_list_wrapper_t::discovery_watcher_callback_list_wrapper_t(
    service_discovery_module &m, service_discovery_cluster_context &c, int64_t index)
    : mod(&m),
      ctx(&c),
      callback_lock(nullptr),
      callbacks(nullptr),
      snapshot_index(index),
      has_insert_snapshot_index(false) {}

service_discovery_module::discovery_watcher_callback_list_wrapper_t::discovery_watcher_callback_list_wrapper_t(
    service_discovery_module &m, service_discovery_cluster_context &c, std::recursive_mutex &lock,
    std::list<discovery_watcher_list_callback_t> &cbks, int64_t index)
    : mod(&m),
      ctx(&c),
      callback_lock(&lock),
      callbacks(&cbks),
      snapshot_index(index),
      has_insert_snapshot_index(false) {}

service_discovery_module::discovery_watcher_callback_list_wrapper_t::~discovery_watcher_callback_list_wrapper_t() {
  if (nullptr != ctx && 0 != snapshot_index && has_insert_snapshot_index) {
    ctx->data_->discovery_watcher_snapshot_index_.erase(snapshot_index);
  }
}

void service_discovery_module::discovery_watcher_callback_list_wrapper_t::operator()(
    const ::atframework::atapp::etcd_response_header &header,
    const ::atframework::atapp::etcd_watcher::response_t &body) {
  if (nullptr == mod) {
    return;
  }
  ctx->data_->last_etcd_event_discovery_header_ = header;

  bool enable_snapshot = body.snapshot && 0 != snapshot_index;
  if (enable_snapshot) {
    if (!has_insert_snapshot_index) {
      ctx->data_->discovery_watcher_snapshot_index_.insert(snapshot_index);
      has_insert_snapshot_index = true;
    }

    // We just accept the earliest watcher as snapshot notifier.
    // So just enable by_id or by_name when initializing and they will has the smallest index.
    if (!ctx->data_->discovery_watcher_snapshot_index_.empty() &&
        *ctx->data_->discovery_watcher_snapshot_index_.begin() != snapshot_index) {
      enable_snapshot = false;
    }
  }

  etcd_discovery_set::node_by_name_type old_names;
  etcd_discovery_set::node_by_id_type old_ids;
  if (enable_snapshot) {
    _collect_old_nodes(*mod, old_names, old_ids);

    for (auto iter = mod->discovery_on_load_snapshot_callbacks_.begin();
         iter != mod->discovery_on_load_snapshot_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }

  // decode data
  for (size_t i = 0; i < body.events.size(); ++i) {
    const ::atframework::atapp::etcd_watcher::event_t &evt_data = body.events[i];
    node_info_t node;
    node.context_ptr = reinterpret_cast<uintptr_t>(ctx);
    etcd_discovery_node::node_version current_node_version;

    unpack(node, evt_data.kv.key.empty() ? evt_data.prev_kv.key : evt_data.kv.key,
           evt_data.kv.value.empty() ? evt_data.prev_kv.value : evt_data.kv.value, true);
    current_node_version.create_revision = std::max(evt_data.prev_kv.create_revision, evt_data.kv.create_revision);
    current_node_version.modify_revision = std::max(evt_data.prev_kv.mod_revision, evt_data.kv.mod_revision);
    current_node_version.version = std::max(evt_data.prev_kv.version, evt_data.kv.version);

    if (node.node_discovery.id() == 0 && node.node_discovery.name().empty()) {
      continue;
    }

    if (enable_snapshot) {
      _remove_old_node_index(node.node_discovery, old_names, old_ids);
    }

    if (evt_data.evt_type == ::atframework::atapp::etcd_watch_event::kDelete) {
      node.action = node_action_t::kDelete;
    } else {
      node.action = node_action_t::kPut;
    }

    mod->update_internal_watcher_event(node, current_node_version);

    if (nullptr == callbacks || nullptr == callback_lock) {
      continue;
    }

    std::lock_guard<std::recursive_mutex> lock_guard{*callback_lock};
    if (callbacks->empty()) {
      continue;
    }

    discovery_watcher_sender_list_t sender(*mod, header, body, evt_data, node);
    for (std::list<discovery_watcher_list_callback_t>::iterator iter = callbacks->begin(); iter != callbacks->end();
         ++iter) {
      if (*iter) {
        (*iter)(std::ref(sender));
      }
    }
  }

  // cleanup old nodes when receive snapshot response
  if (enable_snapshot) {
    service_discovery_module::watcher_internal_access_t::cleanup_old_nodes(*mod, *ctx, old_names, old_ids);

    for (auto iter = mod->discovery_on_snapshot_loaded_callbacks_.begin();
         iter != mod->discovery_on_snapshot_loaded_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }
}

service_discovery_module::topology_watcher_callback_list_wrapper_t::topology_watcher_callback_list_wrapper_t(
    service_discovery_module &m, service_discovery_cluster_context &c, int64_t index)
    : mod(&m),
      ctx(&c),
      callback_lock(nullptr),
      callbacks(nullptr),
      snapshot_index(index),
      has_insert_snapshot_index(false) {}

service_discovery_module::topology_watcher_callback_list_wrapper_t::topology_watcher_callback_list_wrapper_t(
    service_discovery_module &m, service_discovery_cluster_context &c, std::recursive_mutex &lock,
    std::list<topology_watcher_list_callback_t> &cbks, int64_t index)
    : mod(&m),
      ctx(&c),
      callback_lock(&lock),
      callbacks(&cbks),
      snapshot_index(index),
      has_insert_snapshot_index(false) {}

service_discovery_module::topology_watcher_callback_list_wrapper_t::~topology_watcher_callback_list_wrapper_t() {
  if (nullptr != ctx && 0 != snapshot_index && has_insert_snapshot_index) {
    ctx->data_->topology_watcher_snapshot_index_.erase(snapshot_index);
  }
}

void service_discovery_module::topology_watcher_callback_list_wrapper_t::operator()(
    const ::atframework::atapp::etcd_response_header &header,
    const ::atframework::atapp::etcd_watcher::response_t &body) {
  if (nullptr == mod) {
    return;
  }
  ctx->data_->last_etcd_event_topology_header_ = header;

  bool enable_snapshot = body.snapshot && 0 != snapshot_index;
  if (enable_snapshot) {
    if (!has_insert_snapshot_index) {
      ctx->data_->topology_watcher_snapshot_index_.insert(snapshot_index);
      has_insert_snapshot_index = true;
    }

    // We just accept the earliest watcher as snapshot notifier.
    // So just enable by_id or by_name when initializing and they will has the smallest index.
    if (!ctx->data_->topology_watcher_snapshot_index_.empty() &&
        *ctx->data_->topology_watcher_snapshot_index_.begin() != snapshot_index) {
      enable_snapshot = false;
    }
  }

  std::unordered_map<uint64_t, topology_storage_t> old_ids;
  if (enable_snapshot) {
    _collect_old_topology_peers(*mod, old_ids);

    for (auto iter = mod->topology_on_load_snapshot_callbacks_.begin();
         iter != mod->topology_on_load_snapshot_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }

  // decode data
  for (size_t i = 0; i < body.events.size(); ++i) {
    const ::atframework::atapp::etcd_watcher::event_t &evt_data = body.events[i];
    topology_info_t topology_info;
    topology_info.storage.context_ptr = reinterpret_cast<uintptr_t>(ctx);

    unpack(topology_info, evt_data.kv.key.empty() ? evt_data.prev_kv.key : evt_data.kv.key,
           evt_data.kv.value.empty() ? evt_data.prev_kv.value : evt_data.kv.value, true);
    topology_info.storage.version.create_revision =
        std::max(evt_data.prev_kv.create_revision, evt_data.kv.create_revision);
    topology_info.storage.version.modify_revision = std::max(evt_data.prev_kv.mod_revision, evt_data.kv.mod_revision);
    topology_info.storage.version.version = std::max(evt_data.prev_kv.version, evt_data.kv.version);

    if (!topology_info.storage.info || topology_info.storage.info->id() == 0) {
      continue;
    }

    if (enable_snapshot) {
      _remove_old_topology_peer_index(*topology_info.storage.info, old_ids);
    }
    topology_info.action = evt_data.evt_type;

    mod->update_internal_watcher_event(topology_info);

    if (nullptr == callbacks || nullptr == callback_lock) {
      continue;
    }

    std::lock_guard<std::recursive_mutex> lock_guard{*callback_lock};
    if (callbacks->empty()) {
      continue;
    }

    topology_watcher_sender_list_t sender(*mod, header, body, evt_data, topology_info);
    for (std::list<topology_watcher_list_callback_t>::iterator iter = callbacks->begin(); iter != callbacks->end();
         ++iter) {
      if (*iter) {
        (*iter)(std::ref(sender));
      }
    }
  }

  // cleanup old nodes when receive snapshot response
  if (enable_snapshot) {
    service_discovery_module::watcher_internal_access_t::cleanup_old_topology_peers(*mod, *ctx, old_ids);

    for (auto iter = mod->topology_on_snapshot_loaded_callbacks_.begin();
         iter != mod->topology_on_snapshot_loaded_callbacks_.end();) {
      auto copy_iter = iter;
      ++iter;
      if (*copy_iter) {
        (*copy_iter)(*mod);
      }
    }
  }
}

bool service_discovery_module::update_internal_watcher_event(node_info_t &node,
                                                             const etcd_discovery_node::node_version &version) {
  etcd_discovery_node::ptr_t local_cache_by_id = global_discovery_.get_node_by_id(node.node_discovery.id());
  etcd_discovery_node::ptr_t local_cache_by_name = global_discovery_.get_node_by_name(node.node_discovery.name());
  etcd_discovery_node::ptr_t new_inst;

  bool has_event = false;

  if ATFW_UTIL_LIKELY_CONDITION (local_cache_by_id == local_cache_by_name) {
    if (node_action_t::kDelete == node.action) {
      if (local_cache_by_id) {
        local_cache_by_id->update_version(version, true, node.context_ptr);
        global_discovery_.remove_node(local_cache_by_id);

        has_event = true;
      }
    } else {
      if (local_cache_by_id && protobuf_equal(local_cache_by_id->get_discovery_info(), node.node_discovery) &&
          local_cache_by_id->get_context_ptr() == node.context_ptr) {
        local_cache_by_id->update_version(version, false, node.context_ptr);
        return false;
      }

      if (local_cache_by_id) {
        new_inst = local_cache_by_id;
      } else {
        new_inst = atfw::util::memory::make_strong_rc<etcd_discovery_node>();
      }

      new_inst->copy_from(node.node_discovery, version, node.context_ptr);
      global_discovery_.add_node(new_inst);

      has_event = true;
    }
  } else {
    if (node_action_t::kDelete == node.action) {
      if (local_cache_by_id) {
        local_cache_by_id->update_version(version, true, node.context_ptr);
        global_discovery_.remove_node(local_cache_by_id);
        has_event = true;
      }
      if (local_cache_by_name) {
        local_cache_by_name->update_version(version, true, node.context_ptr);
        global_discovery_.remove_node(local_cache_by_name);
        has_event = true;
      }
    } else {
      if ((!local_cache_by_id && 0 != node.node_discovery.id()) ||
          (!local_cache_by_name && !node.node_discovery.name().empty())) {
        has_event = true;
      } else if (local_cache_by_id &&
                 (false == protobuf_equal(local_cache_by_id->get_discovery_info(), node.node_discovery) ||
                  local_cache_by_id->get_context_ptr() != node.context_ptr)) {
        new_inst = local_cache_by_id;
        has_event = true;
      } else if (local_cache_by_name &&
                 (false == protobuf_equal(local_cache_by_name->get_discovery_info(), node.node_discovery) ||
                  local_cache_by_name->get_context_ptr() != node.context_ptr)) {
        new_inst = local_cache_by_name;
        has_event = true;
      }

      if (has_event) {
        if (!new_inst) {
          new_inst = atfw::util::memory::make_strong_rc<etcd_discovery_node>();
        }
        new_inst->copy_from(node.node_discovery, version, node.context_ptr);

        if (local_cache_by_id && local_cache_by_id != new_inst) {
          local_cache_by_id->update_version(version, false, node.context_ptr);
          global_discovery_.remove_node(local_cache_by_id);
        }
        if (local_cache_by_name && local_cache_by_name != new_inst) {
          local_cache_by_name->update_version(version, false, node.context_ptr);
          global_discovery_.remove_node(local_cache_by_name);
        }

        global_discovery_.add_node(new_inst);
      }
    }
  }

  // remove endpoint if got DELETE event
  if (has_event) {
    app *owner = get_app();
    if (node_action_t::kDelete == node.action && nullptr != owner) {
      if (0 != node.node_discovery.id()) {
        owner->remove_endpoint(node.node_discovery.id());
      }
      if (!node.node_discovery.name().empty()) {
        owner->remove_endpoint(node.node_discovery.name());
      }
    }

    if (nullptr != owner) {
      std::lock_guard<std::recursive_mutex> lock_guard{node_event_lock_};

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

bool service_discovery_module::update_internal_watcher_event(topology_info_t &topology_info) {
  if (!topology_info.storage.info) {
    return false;
  }

  atapp_topology_info_ptr_t info_ptr{};
  if (topology_info.action == topology_action_t::kDelete) {
    std::unordered_map<uint64_t, topology_storage_t>::iterator local_cache_iter =
        internal_topology_info_set_.find(topology_info.storage.info->id());
    if (local_cache_iter == internal_topology_info_set_.end()) {
      return false;
    }

    info_ptr = local_cache_iter->second.info;
    internal_topology_info_set_.erase(local_cache_iter);
  } else {
    std::unordered_map<uint64_t, topology_storage_t>::iterator local_cache_iter =
        internal_topology_info_set_.find(topology_info.storage.info->id());
    if (local_cache_iter != internal_topology_info_set_.end()) {
      bool changed = topology_update_version(local_cache_iter->second, topology_info.storage.version, true,
                                             topology_info.storage.context_ptr);

      // 未变化
      if (local_cache_iter->second.info &&
          atapp_topology_equal(*local_cache_iter->second.info, *topology_info.storage.info)) {
        return false;
      }

      // 版本未更新时，视为过期或乱序事件，不触发回调
      if (!changed) {
        return false;
      }
      local_cache_iter->second.info = topology_info.storage.info;
      info_ptr = local_cache_iter->second.info;
    } else {
      internal_topology_info_set_[topology_info.storage.info->id()] = topology_info.storage;
      info_ptr = topology_info.storage.info;
    }
  }

  app *owner = get_app();
  if (owner != nullptr) {
    std::lock_guard<std::recursive_mutex> lock_guard{topology_info_event_lock_};
    owner->trigger_event_on_topology_event(topology_info.action, info_ptr, topology_info.storage.version);

    for (topology_info_event_callback_list_t::iterator iter = topology_info_event_callbacks_.begin();
         iter != topology_info_event_callbacks_.end(); ++iter) {
      if (*iter) {
        (*iter)(topology_info.action, info_ptr, topology_info.storage.version);
      }
    }
  }
  return true;
}

void service_discovery_module::service_discovery_cluster_context::reset_internal_watchers_and_keepalives() {
  if (data_->internal_discovery_watcher_by_name_) {
    etcd_module_.get_etcd_cluster().remove_watcher(data_->internal_discovery_watcher_by_name_);
    data_->internal_discovery_watcher_by_name_.reset();
  }

  if (data_->internal_discovery_watcher_by_id_) {
    etcd_module_.get_etcd_cluster().remove_watcher(data_->internal_discovery_watcher_by_id_);
    data_->internal_discovery_watcher_by_id_.reset();
  }

  if (data_->internal_topology_watcher_) {
    etcd_module_.get_etcd_cluster().remove_watcher(data_->internal_topology_watcher_);
    data_->internal_topology_watcher_.reset();
  }

  data_->internal_discovery_keepalive_actors_.clear();
  data_->internal_topology_keepalive_actors_.clear();
}

LIBATAPP_MACRO_NAMESPACE_END