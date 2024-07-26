// Copyright 2021 Tencent
// Created by owent

#include "atframe/atapp.h"

#include <std/static_assert.h>

#include <libatbus.h>
#include <libatbus_protocol.h>

#include <algorithm/crypto_cipher.h>
#include <algorithm/murmur_hash.h>

#if defined(CRYPTO_USE_OPENSSL) || defined(CRYPTO_USE_LIBRESSL) || defined(CRYPTO_USE_BORINGSSL)
#  include <openssl/ssl.h>
#endif

#include <algorithm/sha.h>
#include <common/file_system.h>
#include <common/string_oprs.h>

#include <cli/shell_font.h>
#include <log/log_sink_file_backend.h>

#if !defined(_WIN32)
#  include <sys/resource.h>
#  include <sys/time.h>
#endif

#include <assert.h>
#include <signal.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#if !(defined(THREAD_TLS_USE_PTHREAD) && THREAD_TLS_USE_PTHREAD) && __cplusplus >= 201103L
#  include <mutex>
#endif

#include "atframe/atapp_conf_rapidjson.h"
#include "atframe/modules/etcd_module.h"

#include "atframe/connectors/atapp_connector_atbus.h"
#include "atframe/connectors/atapp_connector_loopback.h"

#define ATAPP_DEFAULT_STOP_TIMEOUT 30000

namespace atapp {
app *app::last_instance_ = nullptr;

namespace {
enum class atapp_pod_stateful_index : int32_t {
  kUnset = -2,
  kInvalid = -1,
};
}

namespace {
static void _app_close_timer_handle(uv_handle_t *handle) {
  ::atapp::app::timer_ptr_t *ptr = reinterpret_cast<::atapp::app::timer_ptr_t *>(handle->data);
  if (nullptr == ptr) {
    if (nullptr != handle->loop) {
      uv_stop(handle->loop);
    }
    return;
  }

  delete ptr;
}

static std::pair<uint64_t, const char *> make_size_showup(uint64_t sz) {
  const char *unit = "KB";
  if (sz > 102400) {
    sz /= 1024;
    unit = "MB";
  }

  if (sz > 102400) {
    sz /= 1024;
    unit = "GB";
  }

  if (sz > 102400) {
    sz /= 1024;
    unit = "TB";
  }

  return std::pair<uint64_t, const char *>(sz, unit);
}

static uint64_t chrono_to_libuv_duration(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &in,
                                         uint64_t default_value) {
  uint64_t ret = static_cast<uint64_t>(in.seconds() * 1000 + in.nanos() / 1000000);
  if (ret <= 0) {
    ret = default_value;
  }

  return ret;
}

static uint64_t chrono_to_libuv_duration(std::chrono::system_clock::duration in) {
  auto ret = std::chrono::duration_cast<std::chrono::milliseconds>(in).count();
  if (ret < 0) {
    ret = 0;
  }

  return static_cast<uint64_t>(ret);
}

#if defined(THREAD_TLS_USE_PTHREAD) && THREAD_TLS_USE_PTHREAD
static pthread_once_t gt_atapp_global_init_once = PTHREAD_ONCE_INIT;
static void atapp_global_init_once() {
  uv_loop_t loop;
  // Call uv_loop_init() to initialize the global data.
  uv_loop_init(&loop);
  uv_loop_close(&loop);
}
#elif __cplusplus >= 201103L
static std::once_flag gt_atapp_global_init_once;
static void atapp_global_init_once() {
  uv_loop_t loop;
  // Call uv_loop_init() to initialize the global data.
  uv_loop_init(&loop);
  uv_loop_close(&loop);
}
#endif

static std::chrono::milliseconds get_default_system_clock_granularity() {
// This interval will be rounded up to the system clock granularity, which is 1 / (getconf CLK_TCK) on Linux.
// + Most Linux systems have a CLK_TCK of 100, so the interval is rounded to 10ms.
// + Windows usually use 15.6ms
#ifdef _SC_CLK_TCK
  auto clk_tck = sysconf(_SC_CLK_TCK);
  if (clk_tck > 0 && clk_tck < 1000) {
    return std::chrono::milliseconds{1000 / clk_tck};
  }
#endif
  return std::chrono::milliseconds{10};
}

}  // namespace

LIBATAPP_MACRO_API app::message_t::message_t()
    : type(0), message_sequence(0), data(nullptr), data_size(0), metadata(nullptr) {}
LIBATAPP_MACRO_API app::message_t::~message_t() {}

LIBATAPP_MACRO_API app::message_t::message_t(const message_t &other) { (*this) = other; }

LIBATAPP_MACRO_API app::message_t &app::message_t::operator=(const message_t &other) {
  type = other.type;
  message_sequence = other.message_sequence;
  data = other.data;
  data_size = other.data_size;
  metadata = other.metadata;
  return *this;
}

LIBATAPP_MACRO_API app::message_sender_t::message_sender_t() : id(0), remote(nullptr) {}
LIBATAPP_MACRO_API app::message_sender_t::~message_sender_t() {}

LIBATAPP_MACRO_API app::message_sender_t::message_sender_t(const message_sender_t &other) { (*this) = other; }

LIBATAPP_MACRO_API app::message_sender_t &app::message_sender_t::operator=(const message_sender_t &other) {
  id = other.id;
  name = other.name;
  remote = other.remote;
  return *this;
}

LIBATAPP_MACRO_API app::flag_guard_t::flag_guard_t(app &owner, flag_t::type f) : owner_(&owner), flag_(f) {
  if (owner_->check_flag(flag_)) {
    owner_ = nullptr;
    return;
  }

  owner_->set_flag(flag_, true);
}

LIBATAPP_MACRO_API app::flag_guard_t::~flag_guard_t() {
  if (nullptr == owner_) {
    return;
  }

  owner_->set_flag(flag_, false);
}

LIBATAPP_MACRO_API app::app()
    : setup_result_(0),
      ev_loop_(nullptr),
      flags_(0),
      mode_(mode_t::CUSTOM),
      tick_clock_granularity_(std::chrono::milliseconds::zero()) {
  if (nullptr == last_instance_) {
#if defined(OPENSSL_VERSION_NUMBER)
#  if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
#  else
    OPENSSL_init_ssl(0, nullptr);
#  endif
#endif
#ifdef CRYPTO_CIPHER_ENABLED
    util::crypto::cipher::init_global_algorithm();
#endif
  }

  last_instance_ = this;

  memset(pending_signals_, 0, sizeof(pending_signals_));
  conf_.id = 0;
  conf_.execute_path = nullptr;
  conf_.upgrade_mode = false;
  conf_.runtime_pod_stateful_index = static_cast<int32_t>(atapp_pod_stateful_index::kUnset);
  conf_.timer_tick_interval =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds{8});
  conf_.timer_tick_round_timeout =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds{128});
  conf_.timer_reserve_permille = 10;
  conf_.timer_reserve_interval_min =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::microseconds{100});
  conf_.timer_reserve_interval_max =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds{1});
  conf_.timer_reserve_interval_tick = std::chrono::system_clock::duration{conf_.timer_tick_interval.count() *
                                                                          (1000 - conf_.timer_reserve_permille) / 1000};

  util::time::time_utility::update();
  tick_timer_.last_tick_timepoint = util::time::time_utility::sys_now();
  tick_timer_.last_stop_timepoint = std::chrono::system_clock::from_time_t(0);
  tick_timer_.sec = util::time::time_utility::get_sys_now();
  tick_timer_.usec = 0;
  tick_timer_.inner_break = nullptr;
  tick_timer_.tick_compensation = std::chrono::system_clock::duration::zero();

  stats_.last_checkpoint_min = 0;
  stats_.endpoint_wake_count = 0;
  stats_.inner_etcd.sum_error_requests = 0;
  stats_.inner_etcd.continue_error_requests = 0;
  stats_.inner_etcd.sum_success_requests = 0;
  stats_.inner_etcd.continue_success_requests = 0;
  stats_.inner_etcd.sum_create_requests = 0;
  stats_.last_proc_event_count = 0;
  stats_.receive_custom_command_request_count = 0;
  stats_.receive_custom_command_reponse_count = 0;

  atbus_connector_ = add_connector<atapp_connector_atbus>();
  loopback_connector_ = add_connector<atapp_connector_loopback>();

  // inner modules
  internal_module_etcd_ = std::make_shared<atapp::etcd_module>();
  add_module(internal_module_etcd_);

#if defined(THREAD_TLS_USE_PTHREAD) && THREAD_TLS_USE_PTHREAD
  (void)pthread_once(&gt_atapp_global_init_once, atapp_global_init_once);
#elif __cplusplus >= 201103L
  std::call_once(gt_atapp_global_init_once, atapp_global_init_once);
#endif
}

LIBATAPP_MACRO_API app::~app() {
  set_flag(flag_t::DESTROYING, true);
  if (is_inited() && !is_closed()) {
    if (!is_closing()) {
      stop();
    }

    util::time::time_utility::update();
    time_t now = util::time::time_utility::get_sys_now() * 1000 + util::time::time_utility::get_now_usec() / 1000;
    time_t offset = get_origin_configure().timer().stop_timeout().seconds() * 1000;
    offset += get_origin_configure().timer().stop_timeout().nanos() / 1000000;
    if (offset <= 0) {
      offset = 30000;
    }

    time_t timeout = now + offset;
    while (!is_closed() && timeout > now) {
      run_once(0, std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(timeout - now)));

      util::time::time_utility::update();
      now = util::time::time_utility::get_sys_now() * 1000 + util::time::time_utility::get_now_usec() / 1000;
    }
  }

  while (!endpoint_index_by_id_.empty() || !endpoint_index_by_name_.empty()) {
    endpoint_index_by_id_t endpoint_index_by_id;
    endpoint_index_by_name_t endpoint_index_by_name;
    endpoint_index_by_id.swap(endpoint_index_by_id_);
    endpoint_index_by_name.swap(endpoint_index_by_name_);
    for (auto &endpoint : endpoint_index_by_id) {
      if (endpoint.second) {
        atapp_endpoint::internal_accessor::close(*endpoint.second);
      }
    }
    for (auto &endpoint : endpoint_index_by_name) {
      if (endpoint.second) {
        atapp_endpoint::internal_accessor::close(*endpoint.second);
      }
    }
  }
  endpoint_waker_.clear();

  for (module_ptr_t &mod : modules_) {
    if (mod && mod->owner_ == this) {
      mod->deactive();
      mod->on_unbind();
      mod->owner_ = nullptr;
    }
  }

  {
    std::lock_guard<std::recursive_mutex> lock_guard{connectors_lock_};

    auto connectors = connectors_;

    for (auto &connector : connectors_) {
      if (!connector) {
        continue;
      }

      connector->cleanup();
    }

    atbus_connector_.reset();
    loopback_connector_.reset();
    connectors_.clear();
  }

  // reset atbus first, make sure atbus ref count is greater than 0 when reset it
  // some inner async deallocate action will add ref count and we should make sure
  // atbus is not destroying
  if (bus_node_) {
    bus_node_->reset();
    bus_node_.reset();
  }

  // finally events
  app_evt_on_finally();

  // close timer
  close_timer(tick_timer_.tick_timer);
  close_timer(tick_timer_.timeout_timer);

  assert(!tick_timer_.tick_timer);
  assert(!tick_timer_.timeout_timer);

  if (this == last_instance_) {
    last_instance_ = nullptr;
  }

#ifdef CRYPTO_CIPHER_ENABLED
  util::crypto::cipher::cleanup_global_algorithm();
#endif
}

LIBATAPP_MACRO_API int app::run(ev_loop_t *ev_loop, int argc, const char **argv, void *priv_data) {
  if (0 != setup_result_) {
    return setup_result_;
  }

  if (check_flag(flag_t::IN_CALLBACK)) {
    return 0;
  }

  if (is_closed()) {
    return EN_ATAPP_ERR_ALREADY_CLOSED;
  }

  if (false == check_flag(flag_t::INITIALIZED)) {
    int res = init(ev_loop, argc, argv, priv_data);
    if (res < 0) {
      return res;
    }
  }

  if (mode_t::START != mode_) {
    return 0;
  }

  int ret = 0;
  while (!is_closed()) {
    ret = run_inner(UV_RUN_DEFAULT);
  }

  return ret;
}  // namespace atapp

LIBATAPP_MACRO_API int app::init(ev_loop_t *ev_loop, int argc, const char **argv, void *priv_data) {
  if (check_flag(flag_t::INITIALIZED)) {
    return EN_ATAPP_ERR_ALREADY_INITED;
  }

  if (check_flag(flag_t::IN_CALLBACK)) {
    return 0;
  }

  if (check_flag(flag_t::INITIALIZING)) {
    return EN_ATAPP_ERR_RECURSIVE_CALL;
  }
  flag_guard_t init_flag_guard(*this, flag_t::INITIALIZING);

  setup_result_ = 0;

  // update time first
  util::time::time_utility::update();

  // step 1. bind default options
  // step 2. load options from cmd line
  setup_option(argc, argv, priv_data);
  setup_command();

  // step 3. if not in show mode, exit 0
  if (mode_t::INFO == mode_) {
    return 0;
  }
  if (mode_t::HELP == mode_) {
    print_help();
    return 0;
  }

  setup_startup_log();

  // step 4. load options from cmd line
  if (nullptr == ev_loop) {
    ev_loop = uv_default_loop();
  }

  ev_loop_ = ev_loop;
  conf_.bus_conf.ev_loop = ev_loop;
  int ret = reload();
  if (ret < 0) {
    FWLOGERROR("load configure failed");
    return setup_result_ = ret;
  }

  // step 5. if not in start mode, send cmd
  switch (mode_) {
    case mode_t::START: {
      break;
    }
    case mode_t::CUSTOM:
    case mode_t::STOP:
    case mode_t::RELOAD: {
      return send_last_command(ev_loop);
    }
    default: {
      return setup_result_ = 0;
    }
  }

  ret = setup_signal();
  if (ret < 0) {
    FWLOGERROR("setup signal failed");
    write_startup_error_file(ret);
    return setup_result_ = ret;
  }

  // all modules setup
  for (module_ptr_t &mod : modules_) {
    if (mod->is_enabled()) {
      ret = mod->setup(conf_);
      if (ret < 0) {
        FWLOGERROR("setup module {} failed", mod->name());
        write_startup_error_file(ret);
        return setup_result_ = ret;
      }
    }
  }

  // setup timeout timer for initialization
  bool hold_timeout_timer = setup_timeout_timer(conf_.origin.timer().initialize_timeout());
  auto timeout_timer_guard = gsl::finally([hold_timeout_timer, this]() {
    if (hold_timeout_timer) {
      this->close_timer(this->tick_timer_.timeout_timer);
    }
  });

  WLOG_GETCAT(util::log::log_wrapper::categorize_t::DEFAULT)->clear_sinks();
  ret = setup_log();
  if (ret < 0) {
    FWLOGERROR("setup log failed");
    write_startup_error_file(ret);
    return setup_result_ = ret;
  }

  if (check_flag(flag_t::TIMEOUT)) {
    setup_result_ = EN_ATAPP_ERR_OPERATION_TIMEOUT;
    return setup_result_;
  }

  if (setup_tick_timer() < 0) {
    FWLOGERROR("setup timer failed");
    bus_node_.reset();
    write_startup_error_file(EN_ATAPP_ERR_SETUP_TIMER);
    return setup_result_ = EN_ATAPP_ERR_SETUP_TIMER;
  }

  ret = setup_atbus();
  if (ret < 0) {
    FWLOGERROR("setup atbus failed");
    bus_node_.reset();
    write_startup_error_file(ret);
    return setup_result_ = ret;
  }

  // all modules reload
  stats_.module_reload.clear();
  stats_.module_reload.reserve(modules_.size());
  for (module_ptr_t &mod : modules_) {
    if (mod->is_enabled()) {
      std::chrono::system_clock::time_point previous_timepoint = std::chrono::system_clock::now();
      ret = mod->reload();
      std::chrono::system_clock::time_point current_timepoint = std::chrono::system_clock::now();
      stats_data_module_reload_t reload_stats;
      reload_stats.module = mod;
      reload_stats.cost = current_timepoint - previous_timepoint;
      reload_stats.result = ret;
      stats_.module_reload.emplace_back(std::move(reload_stats));
      if (ret < 0) {
        FWLOGERROR("load configure of {} failed", mod->name());
        write_startup_error_file(ret);
        return setup_result_ = ret;
      }
    }
  }

  if (check_flag(flag_t::TIMEOUT)) {
    setup_result_ = EN_ATAPP_ERR_OPERATION_TIMEOUT;
    return setup_result_;
  }

  // all modules init
  size_t inited_mod_idx = 0;
  int mod_init_res = 0;
  for (; mod_init_res >= 0 && inited_mod_idx < modules_.size(); ++inited_mod_idx) {
    if (modules_[inited_mod_idx]->is_enabled()) {
      mod_init_res = modules_[inited_mod_idx]->init();
      if (mod_init_res < 0) {
        FWLOGERROR("initialze {} failed", modules_[inited_mod_idx]->name());
        break;
      }

      modules_[inited_mod_idx]->active();
      ++stats_.last_proc_event_count;

      if (check_flag(flag_t::TIMEOUT)) {
        mod_init_res = EN_ATAPP_ERR_OPERATION_TIMEOUT;
        FWLOGERROR("initialze {} success but atapp timeout", modules_[inited_mod_idx]->name());
        break;
      }
    }
  }

  if (mod_init_res >= 0) {
    // callback of all modules inited
    if (evt_on_all_module_inited_) {
      evt_on_all_module_inited_(*this);
    }

    // maybe timeout in callback
    if (check_flag(flag_t::TIMEOUT)) {
      mod_init_res = EN_ATAPP_ERR_OPERATION_TIMEOUT;
    }
  }

  // cleanup all inited modules if failed
  if (mod_init_res < 0) {
    for (; inited_mod_idx < modules_.size(); --inited_mod_idx) {
      if (modules_[inited_mod_idx]) {
        modules_[inited_mod_idx]->cleanup();
      }

      if (0 == inited_mod_idx) {
        break;
      }
    }

    if (evt_on_all_module_cleaned_) {
      evt_on_all_module_cleaned_(*this);
    }

    write_startup_error_file(mod_init_res);
    return setup_result_ = mod_init_res;
  }

  // write pid file
  if (false == write_pidfile(atbus::node::get_pid())) {
    write_startup_error_file(EN_ATAPP_ERR_WRITE_PID_FILE);
    return EN_ATAPP_ERR_WRITE_PID_FILE;
  }
  if (mode_t::START == mode_) {
    cleanup_startup_error_file();
  }

  set_flag(flag_t::STOPPED, false);
  set_flag(flag_t::STOPING, false);
  set_flag(flag_t::INITIALIZED, true);
  set_flag(flag_t::RUNNING, true);

  // notify all module to get ready
  for (inited_mod_idx = 0; inited_mod_idx < modules_.size(); ++inited_mod_idx) {
    if (modules_[inited_mod_idx]->is_enabled()) {
      modules_[inited_mod_idx]->ready();
    }
  }

  return EN_ATAPP_ERR_SUCCESS;
}  // namespace atapp

LIBATAPP_MACRO_API int app::run_noblock(uint64_t max_event_count) {
  uint64_t evt_count = 0;
  int ret = 0;
  do {
    ret = run_inner(UV_RUN_NOWAIT);
    if (ret < 0) {
      break;
    }

    if (0 == stats_.last_proc_event_count) {
      break;
    }

    evt_count += stats_.last_proc_event_count;
  } while (0 == max_event_count || evt_count < max_event_count);

  return ret;
}

LIBATAPP_MACRO_API int app::run_once(uint64_t min_event_count, std::chrono::system_clock::duration timeout_duration) {
  ev_loop_t *loop = get_evloop();
  if (nullptr == loop) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  uint64_t evt_count = 0;
  int ret = 0;

  util::time::time_utility::raw_time_t timeout;
  if (timeout_duration.count() > 0) {
    util::time::time_utility::update();
    timeout = util::time::time_utility::sys_now() + timeout_duration;
  }

  do {
    if (timeout_duration.count() > 0) {
      if (nullptr == tick_timer_.inner_break) {
        tick_timer_.inner_break = &timeout;
      } else if (timeout < *tick_timer_.inner_break) {
        tick_timer_.inner_break = &timeout;
      }
    }

    ret = run_inner(UV_RUN_ONCE);
    if (ret < 0) {
      break;
    }

    evt_count += stats_.last_proc_event_count;

    if (timeout_duration.count() > 0) {
      util::time::time_utility::update();
      if (timeout <= util::time::time_utility::sys_now()) {
        break;
      }
    }
  } while (evt_count < min_event_count);

  if (&timeout == tick_timer_.inner_break) {
    tick_timer_.inner_break = nullptr;
  }

  return ret;
}

LIBATAPP_MACRO_API bool app::is_inited() const noexcept { return check_flag(flag_t::INITIALIZED); }

LIBATAPP_MACRO_API bool app::is_running() const noexcept { return check_flag(flag_t::RUNNING); }

LIBATAPP_MACRO_API bool app::is_closing() const noexcept { return check_flag(flag_t::STOPING); }

LIBATAPP_MACRO_API bool app::is_closed() const noexcept { return check_flag(flag_t::STOPPED); }

static bool guess_configure_file_is_yaml(const std::string &file_path) {
  std::fstream file;
  file.open(file_path.c_str(), std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    return false;
  }

  std::string line;
  bool is_first_line = true;
  while (std::getline(file, line)) {
    if (is_first_line) {
      const unsigned char utf8_bom[3] = {0xef, 0xbb, 0xbf};
      // Skip UTF-8 BOM
      if (line.size() > 3 && 0 == memcmp(line.c_str(), utf8_bom, 3)) {
        line = line.substr(3);
      }
    }

    const char *begin = line.c_str();
    const char *end = line.c_str() + line.size();
    for (; *begin && begin < end; ++begin) {
      // YAML: Key: Value
      //   ini section and ini key can not contain ':'
      if (*begin == ':') {
        return true;
      }

      // ini section: [SECTION NAME...]
      //   YAML root node of configure file can not be sequence
      if (*begin == '[') {
        return false;
      }

      // ini: Key = Value
      //   YAML key can not contain '='
      if (*begin == '=') {
        return false;
      }
    }
  }

  return false;
}

static bool reload_all_configure_files(app::yaml_conf_map_t &yaml_map, util::config::ini_loader &conf_loader,
                                       atbus::detail::auto_select_set<std::string>::type &loaded_files,
                                       std::list<std::string> &pending_load_files) {
  bool ret = true;
  size_t conf_external_loaded_index;

  while (!pending_load_files.empty()) {
    std::string file_rule = pending_load_files.front();
    pending_load_files.pop_front();

    if (file_rule.empty()) {
      continue;
    }

    std::string::size_type colon = std::string::npos;
    // Skip windows absolute drive
    if (file_rule.size() < 2 || file_rule[1] != ':') {
      colon = file_rule.find(':');
    }
    bool is_yaml = false;

    if (colon != std::string::npos) {
      is_yaml = (0 == UTIL_STRFUNC_STRNCASE_CMP("yml", file_rule.c_str(), colon)) ||
                (0 == UTIL_STRFUNC_STRNCASE_CMP("yaml", file_rule.c_str(), colon));
      file_rule = file_rule.substr(colon + 1);
    }

    if (loaded_files.end() != loaded_files.find(file_rule)) {
      continue;
    }
    loaded_files.insert(file_rule);

    if (colon == std::string::npos) {
      is_yaml = guess_configure_file_is_yaml(file_rule);
    }

    if (is_yaml) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      try {
#endif
        yaml_map[file_rule] = YAML::LoadAllFromFile(file_rule);
        std::vector<YAML::Node> &nodes = yaml_map[file_rule];
        if (nodes.empty()) {
          yaml_map.erase(file_rule);
          continue;
        }

        // external files
        for (size_t i = 0; i < nodes.size(); ++i) {
          const YAML::Node atapp_node = nodes[i]["atapp"];
          if (!atapp_node || !atapp_node.IsMap()) {
            continue;
          }
          const YAML::Node atapp_config = atapp_node["config"];
          if (!atapp_config || !atapp_config.IsMap()) {
            continue;
          }
          const YAML::Node atapp_external = atapp_node["external"];
          if (!atapp_external) {
            continue;
          }

          if (atapp_config.IsSequence()) {
            for (size_t j = 0; j < atapp_config.size(); ++j) {
              const YAML::Node atapp_external_ele = atapp_config[j];
              if (!atapp_external_ele) {
                continue;
              }
              if (!atapp_external_ele.IsScalar()) {
                continue;
              }
              if (atapp_external_ele.Scalar().empty()) {
                continue;
              }
              pending_load_files.push_back(atapp_external_ele.Scalar());
            }
          } else if (atapp_config.IsScalar()) {
            if (!atapp_config.Scalar().empty()) {
              pending_load_files.push_back(atapp_config.Scalar());
            }
          }
        }
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      } catch (YAML::ParserException &e) {
        FWLOGERROR("load configure file {} failed.{}", file_rule, e.what());
        ret = false;
      } catch (YAML::BadSubscript &e) {
        FWLOGERROR("load configure file {} failed.{}", file_rule, e.what());
        ret = false;
      } catch (...) {
        FWLOGERROR("load configure file {} failed.", file_rule);
        ret = false;
      }
#endif
    } else {
      conf_external_loaded_index = conf_loader.get_node("atapp.config.external").size();
      if (conf_loader.load_file(file_rule.c_str(), false) < 0) {
        FWLOGERROR("load configure file {} failed", file_rule);
        ret = false;
        continue;
      } else {
        FWLOGINFO("load configure file {} success", file_rule);
      }

      // external files
      util::config::ini_value &external_paths = conf_loader.get_node("atapp.config.external");
      for (size_t i = conf_external_loaded_index; i < external_paths.size(); ++i) {
        pending_load_files.push_back(external_paths.as_cpp_string(i));
      }
    }
  }

  return ret;
}

LIBATAPP_MACRO_API int app::reload() {
  atbus::adapter::loop_t *old_loop = conf_.bus_conf.ev_loop;

  FWLOGWARNING("============ start to load configure ============");
  // step 1. reset configure
  cfg_loader_.clear();
  yaml_loader_.clear();

  // step 2. load configures from file or environment variables
  atbus::detail::auto_select_set<std::string>::type loaded_files;
  std::list<std::string> pending_load_files;
  if (!conf_.conf_file.empty()) {
    pending_load_files.push_back(conf_.conf_file);
  }
  if (!reload_all_configure_files(yaml_loader_, cfg_loader_, loaded_files, pending_load_files)) {
    print_help();
    return EN_ATAPP_ERR_LOAD_CONFIGURE_FILE;
  }

  // apply ini configure
  apply_configure();

  // Check configure
  if (0 == get_app_id() && get_app_name().empty()) {
    FWLOGERROR("Invalid configure file and can not load atapp id and name from environment");
    print_help();
    return EN_ATAPP_ERR_MISSING_CONFIGURE_FILE;
  }

  // reuse ev loop if not configued
  if (nullptr == conf_.bus_conf.ev_loop) {
    conf_.bus_conf.ev_loop = old_loop;
  }

  // step 5. if not in start mode, return
  if (mode_t::START != mode_) {
    return 0;
  }

  if (is_running()) {
    // step 6. reset log
    setup_log();

    // step 7. if inited, let all modules reload
    stats_.module_reload.clear();
    stats_.module_reload.reserve(modules_.size());
    for (module_ptr_t &mod : modules_) {
      if (mod->is_enabled()) {
        std::chrono::system_clock::time_point previous_timepoint = std::chrono::system_clock::now();
        int res = mod->reload();
        std::chrono::system_clock::time_point current_timepoint = std::chrono::system_clock::now();
        stats_data_module_reload_t reload_stats;
        reload_stats.module = mod;
        reload_stats.cost = current_timepoint - previous_timepoint;
        reload_stats.result = res;
        stats_.module_reload.emplace_back(std::move(reload_stats));
        if (res < 0) {
          FWLOGERROR("load configure of {} failed", mod->name());
        }
      }
    }

    if (internal_module_etcd_) {
      internal_module_etcd_->set_maybe_update_keepalive_value();
    }
  }

  FWLOGWARNING("------------ load configure done ------------");
  return 0;
}

LIBATAPP_MACRO_API int app::stop() {
  if (check_flag(flag_t::STOPING)) {
    FWLOGWARNING("============= recall stop after some event action(s) finished =============");
  } else {
    FWLOGWARNING("============ receive stop signal and ready to stop all modules ============");
  }

  tick_timer_.last_stop_timepoint = tick_timer_.last_tick_timepoint;

  // step 1. set stop flag.
  // bool is_stoping = set_flag(flag_t::STOPING, true);
  set_flag(flag_t::STOPING, true);

  // step 2. stop libuv and return from uv_run
  // if (!is_stoping) {
  ev_loop_t *loop = get_evloop();
  if (bus_node_ && nullptr != loop) {
    uv_stop(loop);
  }
  // }
  return 0;
}

LIBATAPP_MACRO_API int app::tick() {
  if (check_flag(flag_t::IN_TICK)) {
    return 0;
  }
  flag_guard_t in_tick_guard(*this, flag_t::IN_TICK);

  int32_t active_count;
  util::time::time_utility::update();
  // record start time point
  util::time::time_utility::raw_time_t start_tp = util::time::time_utility::sys_now();
  util::time::time_utility::raw_time_t end_tp = start_tp;

  end_tp += conf_.timer_tick_round_timeout;

  tick_timer_.last_tick_timepoint = util::time::time_utility::sys_now();
  do {
    tick_timer_.sec = util::time::time_utility::get_sys_now();
    tick_timer_.usec = static_cast<time_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(util::time::time_utility::sys_now() -
                                                              std::chrono::system_clock::from_time_t(tick_timer_.sec))
            .count());

    active_count = 0;
    int res;
    // step 1. proc available modules
    for (module_ptr_t &mod : modules_) {
      if (mod->is_enabled() && mod->is_actived()) {
        res = mod->tick();
        if (res < 0) {
          FWLOGERROR("module {} run tick and return {}", mod->name(), res);
        } else {
          active_count += res;
        }
      }
    }

    // step 2. proc atbus
    if (bus_node_ && ::atbus::node::state_t::CREATED != bus_node_->get_state()) {
      res = bus_node_->proc(tick_timer_.sec, tick_timer_.usec);
      if (res < 0) {
        FWLOGERROR("atbus node run tick and return {}", res);
      } else {
        active_count += res;
      }
    }

    // step 3. process inner events
    // This should be called at last, because it only concern time
    util::time::time_utility::update();
    active_count += process_inner_events(util::time::time_utility::sys_now() + conf_.timer_tick_interval);

    // only tick time less than tick interval will run loop again
    if (active_count > 0) {
      stats_.last_proc_event_count += static_cast<uint64_t>(active_count);
    }
    util::time::time_utility::update();
  } while (active_count > 0 && util::time::time_utility::sys_now() < end_tp);

  ev_loop_t *loop = get_evloop();
  // if is stoping, quit loop every tick
  if (nullptr != loop) {
    if (check_flag(flag_t::STOPING)) {
      std::chrono::system_clock::duration stop_interval =
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
              std::chrono::seconds{conf_.origin.timer().stop_interval().seconds()});
      stop_interval += std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::nanoseconds{conf_.origin.timer().stop_interval().nanos()});
      std::chrono::system_clock::duration stop_offset =
          tick_timer_.last_tick_timepoint - tick_timer_.last_stop_timepoint;
      if ((stop_offset.count() > 0 && stop_offset > stop_interval) ||
          (stop_offset.count() < 0 && -stop_offset > stop_interval)) {
        tick_timer_.last_stop_timepoint = tick_timer_.last_tick_timepoint;
        uv_stop(loop);
      }
    }

    if (nullptr != tick_timer_.inner_break && tick_timer_.last_tick_timepoint >= *tick_timer_.inner_break) {
      tick_timer_.inner_break = nullptr;
      uv_stop(loop);
    }
  }

  // stat log
  do {
    time_t now_min = util::time::time_utility::get_sys_now() / util::time::time_utility::MINITE_SECONDS;
    if (now_min == stats_.last_checkpoint_min) {
      break;
    }

    time_t last_min = stats_.last_checkpoint_min;
    stats_.last_checkpoint_min = now_min;
    if (last_min + 1 == now_min) {
      uv_rusage_t last_usage;
      memcpy(&last_usage, &stats_.last_checkpoint_usage, sizeof(uv_rusage_t));
      if (0 != uv_getrusage(&stats_.last_checkpoint_usage)) {
        break;
      }
      auto offset_usr = stats_.last_checkpoint_usage.ru_utime.tv_sec - last_usage.ru_utime.tv_sec;
      auto offset_sys = stats_.last_checkpoint_usage.ru_stime.tv_sec - last_usage.ru_stime.tv_sec;
      offset_usr *= 1000000;
      offset_sys *= 1000000;
      offset_usr += stats_.last_checkpoint_usage.ru_utime.tv_usec - last_usage.ru_utime.tv_usec;
      offset_sys += stats_.last_checkpoint_usage.ru_stime.tv_usec - last_usage.ru_stime.tv_usec;

      std::pair<uint64_t, const char *> max_rss = make_size_showup(last_usage.ru_maxrss);
#ifdef WIN32
      FWLOGINFO(
          "[STATISTICS]: {} CPU usage: user {:02.3}%, sys {:02.3}%, max rss: {}{}, page faults: {}", get_app_name(),
          offset_usr / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
          offset_sys / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
          max_rss.first, max_rss.second, last_usage.ru_majflt);
#else
      std::pair<uint64_t, const char *> ru_ixrss = make_size_showup(last_usage.ru_ixrss);
      std::pair<uint64_t, const char *> ru_idrss = make_size_showup(last_usage.ru_idrss);
      std::pair<uint64_t, const char *> ru_isrss = make_size_showup(last_usage.ru_isrss);
      FWLOGINFO(
          "[STATISTICS]: {} CPU usage: user {:02.3}%, sys {:02.3}%, max rss: {}{}, shared size: {}{}, unshared data "
          "size: "
          "{}{}, unshared "
          "stack size: {}{}, page faults: {}",
          get_app_name(),
          offset_usr / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
          offset_sys / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f),  // usec and add %
          max_rss.first, max_rss.second, ru_ixrss.first, ru_ixrss.second, ru_idrss.first, ru_idrss.second,
          ru_isrss.first, ru_isrss.second, last_usage.ru_majflt);
      if (internal_module_etcd_) {
        const ::atapp::etcd_cluster::stats_t &current = internal_module_etcd_->get_raw_etcd_ctx().get_stats();
        FWLOGINFO(
            "\tetcd module(last minite): request count: {}, failed request: {}, continue failed: {}, success "
            "request: "
            "{}, continue success request {}",
            current.sum_create_requests - stats_.inner_etcd.sum_create_requests,
            current.sum_error_requests - stats_.inner_etcd.sum_error_requests,
            current.continue_error_requests - stats_.inner_etcd.continue_error_requests,
            current.sum_success_requests - stats_.inner_etcd.sum_success_requests,
            current.continue_success_requests - stats_.inner_etcd.continue_success_requests);
        FWLOGINFO(
            "\tetcd module(sum): request count: {}, failed request: {}, continue failed: {}, success request: "
            "{}, continue success request {}",
            current.sum_create_requests, current.sum_error_requests, current.continue_error_requests,
            current.sum_success_requests, current.continue_success_requests);
        stats_.inner_etcd = current;
      }

      FWLOGINFO("\tendpoint wake count: {}, by_id index size: {}, by_name index size: {}, waker size: {}",
                stats_.endpoint_wake_count, endpoint_index_by_id_.size(), endpoint_index_by_name_.size(),
                endpoint_waker_.size());
      stats_.endpoint_wake_count = 0;
#endif
    } else {
      uv_getrusage(&stats_.last_checkpoint_usage);
      if (internal_module_etcd_) {
        stats_.inner_etcd = internal_module_etcd_->get_raw_etcd_ctx().get_stats();
      }
    }

    if (nullptr != loop) {
      uv_stop(loop);
    }
  } while (false);

  return active_count;
}

LIBATAPP_MACRO_API app::app_id_t app::get_id() const noexcept { return conf_.id; }

LIBATAPP_MACRO_API app::ev_loop_t *app::get_evloop() { return ev_loop_; }

LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in) const noexcept {
  return convert_app_id_by_string(id_in, conf_.id_mask);
}

LIBATAPP_MACRO_API std::string app::convert_app_id_to_string(app_id_t id_in, bool hex) const noexcept {
  return convert_app_id_to_string(id_in, conf_.id_mask, hex);
}

LIBATAPP_MACRO_API void app::add_module(module_ptr_t app_module) {
  if (this == app_module->owner_) {
    return;
  }

  assert(nullptr == app_module->owner_);
  if (nullptr == app_module->owner_) {
    modules_.push_back(app_module);
    app_module->owner_ = this;
    app_module->on_bind();
  }
}

LIBATAPP_MACRO_API util::cli::cmd_option_ci::ptr_type app::get_command_manager() {
  if (!cmd_handler_) {
    return cmd_handler_ = util::cli::cmd_option_ci::create();
  }

  return cmd_handler_;
}

LIBATAPP_MACRO_API util::cli::cmd_option::ptr_type app::get_option_manager() {
  if (!app_option_) {
    return app_option_ = util::cli::cmd_option::create();
  }

  return app_option_;
}

LIBATAPP_MACRO_API bool app::is_current_upgrade_mode() const noexcept { return conf_.upgrade_mode; }

LIBATAPP_MACRO_API util::network::http_request::curl_m_bind_ptr_t app::get_shared_curl_multi_context() const noexcept {
  if UTIL_LIKELY_CONDITION (internal_module_etcd_) {
    return internal_module_etcd_->get_shared_curl_multi_context();
  }

  return nullptr;
}

LIBATAPP_MACRO_API void app::set_app_version(const std::string &ver) { conf_.app_version = ver; }

LIBATAPP_MACRO_API const std::string &app::get_app_version() const noexcept { return conf_.app_version; }

LIBATAPP_MACRO_API void app::set_build_version(const std::string &ver) { build_version_ = ver; }

LIBATAPP_MACRO_API const std::string &app::get_build_version() const noexcept {
  if (build_version_.empty()) {
    std::stringstream ss;
    if (get_app_version().empty()) {
      ss << "1.0.0.0 - based on libatapp " << LIBATAPP_VERSION << std::endl;
    } else {
      ss << get_app_version() << " - based on libatapp " << LIBATAPP_VERSION << std::endl;
    }

    const size_t key_padding = 20;

#ifdef __DATE__
    ss << std::setw(key_padding) << "Build Time: " << __DATE__;
#  ifdef __TIME__
    ss << " " << __TIME__;
#  endif
    ss << std::endl;
#endif

#if defined(PROJECT_SCM_VERSION) || defined(PROJECT_SCM_NAME) || defined(PROJECT_SCM_BRANCH)
    ss << std::setw(key_padding) << "Build SCM:";
#  ifdef PROJECT_SCM_NAME
    ss << " " << PROJECT_SCM_NAME;
#  endif
#  ifdef PROJECT_SCM_BRANCH
    ss << " branch " << PROJECT_SCM_BRANCH;
#  endif
#  ifdef PROJECT_SCM_VERSION
    ss << " commit " << PROJECT_SCM_VERSION;
#  endif
#endif

#if defined(_MSC_VER)
    ss << std::setw(key_padding) << "Build Compiler: MSVC ";
#  ifdef _MSC_FULL_VER
    ss << _MSC_FULL_VER;
#  else
    ss << _MSC_VER;
#  endif

#  ifdef _MSVC_LANG
    ss << " with standard " << _MSVC_LANG;
#  endif
    ss << std::endl;

#elif defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
    ss << std::setw(key_padding) << "Build Compiler: ";
#  if defined(__clang__)
    ss << "clang ";
#  else
    ss << "gcc ";
#  endif

#  if defined(__clang_version__)
    ss << __clang_version__;
#  elif defined(__VERSION__)
    ss << __VERSION__;
#  endif
#  if defined(__OPTIMIZE__)
    ss << " optimize level " << __OPTIMIZE__;
#  endif
#  if defined(__STDC_VERSION__)
    ss << " C standard " << __STDC_VERSION__;
#  endif
#  if defined(__cplusplus) && __cplusplus > 1
    ss << " C++ standard " << __cplusplus;
#  endif
    ss << std::endl;
#endif

    build_version_ = ss.str();
  }

  return build_version_;
}

LIBATAPP_MACRO_API app::app_id_t app::get_app_id() const noexcept { return conf_.id; }

LIBATAPP_MACRO_API const std::string &app::get_app_name() const noexcept { return conf_.origin.name(); }

LIBATAPP_MACRO_API const std::string &app::get_app_identity() const noexcept { return conf_.origin.identity(); }

LIBATAPP_MACRO_API const std::string &app::get_type_name() const noexcept { return conf_.origin.type_name(); }

LIBATAPP_MACRO_API app::app_id_t app::get_type_id() const noexcept {
  return static_cast<app::app_id_t>(conf_.origin.type_id());
}

LIBATAPP_MACRO_API const std::string &app::get_hash_code() const noexcept { return conf_.hash_code; }

LIBATAPP_MACRO_API std::shared_ptr<atbus::node> app::get_bus_node() { return bus_node_; }
LIBATAPP_MACRO_API const std::shared_ptr<atbus::node> app::get_bus_node() const noexcept { return bus_node_; }

LIBATAPP_MACRO_API void app::enable_fallback_to_atbus_connector() { set_flag(flag_t::DISABLE_ATBUS_FALLBACK, false); }

LIBATAPP_MACRO_API void app::disable_fallback_to_atbus_connector() { set_flag(flag_t::DISABLE_ATBUS_FALLBACK, true); }

LIBATAPP_MACRO_API bool app::is_fallback_to_atbus_connector_enabled() const noexcept {
  return !check_flag(flag_t::DISABLE_ATBUS_FALLBACK);
}

LIBATAPP_MACRO_API util::time::time_utility::raw_time_t app::get_last_tick_time() const noexcept {
  return tick_timer_.last_tick_timepoint;
}

LIBATAPP_MACRO_API util::config::ini_loader &app::get_configure_loader() { return cfg_loader_; }
LIBATAPP_MACRO_API const util::config::ini_loader &app::get_configure_loader() const noexcept { return cfg_loader_; }

LIBATAPP_MACRO_API std::chrono::system_clock::duration app::get_configure_timer_interval() const noexcept {
  return conf_.timer_tick_interval;
}

LIBATAPP_MACRO_API int64_t app::get_configure_timer_reserve_permille() const noexcept {
  return conf_.timer_reserve_permille;
}

LIBATAPP_MACRO_API std::chrono::system_clock::duration app::get_configure_timer_reserve_tick() const noexcept {
  return conf_.timer_reserve_interval_tick;
}

LIBATAPP_MACRO_API app::yaml_conf_map_t &app::get_yaml_loaders() { return yaml_loader_; }
LIBATAPP_MACRO_API const app::yaml_conf_map_t &app::get_yaml_loaders() const noexcept { return yaml_loader_; }

LIBATAPP_MACRO_API void app::parse_configures_into(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                   gsl::string_view configure_prefix_path,
                                                   gsl::string_view load_environemnt_prefix,
                                                   configure_key_set *existed_keys) const {
  configure_key_set autocomplete_default_values;
  if (nullptr == existed_keys) {
    existed_keys = &autocomplete_default_values;
  }
  if (!load_environemnt_prefix.empty()) {
    environment_loader_dump_to(load_environemnt_prefix, dst, existed_keys);
  }

  if (!configure_prefix_path.empty()) {
    util::config::ini_value::ptr_t cfg_value = cfg_loader_.get_root_node().get_child_by_path(configure_prefix_path);
    if (cfg_value) {
      ini_loader_dump_to(*cfg_value, dst, existed_keys);
    }
  } else {
    ini_loader_dump_to(cfg_loader_.get_root_node(), dst, existed_keys);
  }

  for (yaml_conf_map_t::const_iterator iter = yaml_loader_.begin(); iter != yaml_loader_.end(); ++iter) {
    for (size_t i = 0; i < iter->second.size(); ++i) {
      yaml_loader_dump_to(yaml_loader_get_child_by_path(iter->second[i], configure_prefix_path), dst, existed_keys);
    }
  }

  // Dump default values
  default_loader_dump_to(dst, *existed_keys);
}

LIBATAPP_MACRO_API void app::parse_log_configures_into(atapp::protocol::atapp_log &dst,
                                                       std::vector<gsl::string_view> configure_prefix_paths,
                                                       gsl::string_view load_environemnt_prefix,
                                                       configure_key_set *existed_keys) const noexcept {
  dst.Clear();
  configure_key_set autocomplete_default_values;
  if (nullptr == existed_keys) {
    existed_keys = &autocomplete_default_values;
  }
  if (!load_environemnt_prefix.empty()) {
    parse_environment_log_categories_into(dst, load_environemnt_prefix, existed_keys);
  }

  // load log configure - ini/conf
  parse_ini_log_categories_into(dst, configure_prefix_paths, existed_keys);

  // load log configure - yaml
  parse_yaml_log_categories_into(dst, configure_prefix_paths, existed_keys);

  std::sort(dst.mutable_category()->begin(), dst.mutable_category()->end(),
            [](const ::atapp::protocol::atapp_log_category &l, const ::atapp::protocol::atapp_log_category &r) {
              return l.index() < r.index();
            });

  // Dump default values
  default_loader_dump_to(dst, *existed_keys);
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_configure &app::get_origin_configure() const noexcept {
  return conf_.origin;
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_log &app::get_log_configure() const noexcept { return conf_.log; }

LIBATAPP_MACRO_API const atapp::protocol::atapp_metadata &app::get_metadata() const noexcept { return conf_.metadata; }

LIBATAPP_MACRO_API const atapp::protocol::atapp_runtime &app::get_runtime_configure() const noexcept {
  return conf_.runtime;
}

LIBATAPP_MACRO_API atapp::protocol::atapp_runtime &app::mutable_runtime_configure() { return conf_.runtime; }

LIBATAPP_MACRO_API void app::set_api_version(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.api_version().c_str(), conf_.metadata.api_version().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_api_version(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_kind(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.kind().c_str(), conf_.metadata.kind().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_kind(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_group(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.group().c_str(), conf_.metadata.group().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_group(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_metadata_name(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.name().c_str(), conf_.metadata.name().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_name(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_metadata_namespace_name(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.namespace_name().c_str(), conf_.metadata.namespace_name().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_namespace_name(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_metadata_uid(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.uid().c_str(), conf_.metadata.uid().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_uid(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_metadata_service_subset(gsl::string_view value) {
  if (gsl::string_view(conf_.metadata.service_subset().c_str(), conf_.metadata.service_subset().size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  conf_.metadata.set_service_subset(value.data(), value.size());
}

LIBATAPP_MACRO_API void app::set_metadata_label(gsl::string_view key, gsl::string_view value) {
  if (key.empty()) {
    return;
  }

  auto labels = conf_.metadata.mutable_labels();
  if (nullptr == labels) {
    return;
  }

  std::string key_string = static_cast<std::string>(key);
  auto iter = labels->find(key_string);
  if (iter != labels->end() && gsl::string_view(iter->second.c_str(), iter->second.size()) == value) {
    return;
  }

  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_metadata();
  }

  (*labels)[key_string] = static_cast<std::string>(value);
}

LIBATAPP_MACRO_API int32_t app::get_runtime_stateful_pod_index() const noexcept {
  if (static_cast<int32_t>(atapp_pod_stateful_index::kUnset) != conf_.runtime_pod_stateful_index) {
    return conf_.runtime_pod_stateful_index;
  }

  if (conf_.runtime.spec().node_name().empty()) {
    return static_cast<int32_t>(atapp_pod_stateful_index::kInvalid);
  }

  const std::string &node_name = conf_.runtime.spec().node_name();
  std::string::size_type find_res = node_name.rfind('-');
  if (find_res == std::string::npos || find_res + 1 >= node_name.size()) {
    const_cast<app *>(this)->conf_.runtime_pod_stateful_index =
        static_cast<int32_t>(atapp_pod_stateful_index::kInvalid);
  } else {
    ++find_res;
    for (std::string::size_type i = 0; i < node_name.size(); ++i) {
      if (node_name[i] < '0' || node_name[i] > '9') {
        const_cast<app *>(this)->conf_.runtime_pod_stateful_index =
            static_cast<int32_t>(atapp_pod_stateful_index::kInvalid);
        return conf_.runtime_pod_stateful_index;
      }
    }

    const_cast<app *>(this)->conf_.runtime_pod_stateful_index =
        util::string::to_int<int32_t>(node_name.c_str() + find_res);
  }

  return conf_.runtime_pod_stateful_index;
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_area &app::get_area() const noexcept { return conf_.origin.area(); }

LIBATAPP_MACRO_API atapp::protocol::atapp_area &app::mutable_area() {
  if (internal_module_etcd_) {
    internal_module_etcd_->set_maybe_update_keepalive_area();
  }
  return *conf_.origin.mutable_area();
}

LIBATAPP_MACRO_API util::time::time_utility::raw_time_t::duration app::get_configure_message_timeout() const noexcept {
  const google::protobuf::Duration &dur = conf_.origin.timer().message_timeout();
  if (dur.seconds() <= 0 && dur.nanos() <= 0) {
    return std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(5));
  }

  util::time::time_utility::raw_time_t::duration ret =
      std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(dur.seconds()));
  ret += std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(dur.nanos()));
  return ret;
}

LIBATAPP_MACRO_API void app::pack(atapp::protocol::atapp_discovery &out) const {
  out.set_id(get_app_id());
  out.set_name(get_app_name());
  out.set_hostname(atbus::node::get_hostname());
  out.set_pid(atbus::node::get_pid());
  out.set_identity(get_app_identity());

  out.set_hash_code(get_hash_code());
  out.set_type_id(get_type_id());
  out.set_type_name(get_type_name());
  if (conf_.origin.has_area()) {
    out.mutable_area()->CopyFrom(conf_.origin.area());
  }
  out.set_version(get_app_version());
  // out.set_custom_data(get_conf_custom_data());
  out.mutable_gateways()->Reserve(conf_.origin.bus().gateways().size());
  for (int i = 0; i < conf_.origin.bus().gateways().size(); ++i) {
    atapp::protocol::atapp_gateway *gateway = out.add_gateways();
    if (nullptr != gateway) {
      gateway->CopyFrom(conf_.origin.bus().gateways(i));
    }
  }

  out.mutable_metadata()->CopyFrom(get_metadata());

  if (bus_node_) {
    const std::vector<atbus::endpoint_subnet_conf> &subnets = bus_node_->get_conf().subnets;
    for (size_t i = 0; i < subnets.size(); ++i) {
      atapp::protocol::atbus_subnet_range *subset = out.add_atbus_subnets();
      if (nullptr == subset) {
        FWLOGERROR("pack configures for {}(0x{:x}) but malloc atbus_subnet_range failed", get_app_name(), get_app_id());
        break;
      }

      subset->set_id_prefix(subnets[i].id_prefix);
      subset->set_mask_bits(subnets[i].mask_bits);
    }

    out.mutable_listen()->Reserve(static_cast<int>(bus_node_->get_listen_list().size()));
    // std::list<std::string>
    for (std::list<std::string>::const_iterator iter = bus_node_->get_listen_list().begin();
         iter != bus_node_->get_listen_list().end(); ++iter) {
      out.add_listen(*iter);
    }
    out.set_atbus_protocol_version(bus_node_->get_protocol_version());
    out.set_atbus_protocol_min_version(bus_node_->get_protocol_minimal_version());
  } else {
    out.mutable_listen()->Reserve(conf_.origin.bus().listen().size());
    for (int i = 0; i < conf_.origin.bus().listen().size(); ++i) {
      out.add_listen(conf_.origin.bus().listen(i));
    }
    out.set_atbus_protocol_version(atbus::protocol::ATBUS_PROTOCOL_VERSION);
    out.set_atbus_protocol_min_version(atbus::protocol::ATBUS_PROTOCOL_MINIMAL_VERSION);
  }
}

LIBATAPP_MACRO_API std::shared_ptr<::atapp::etcd_module> app::get_etcd_module() const noexcept {
  return internal_module_etcd_;
}

LIBATAPP_MACRO_API const etcd_discovery_set &app::get_global_discovery() const noexcept {
  if (internal_module_etcd_) {
    return internal_module_etcd_->get_global_discovery();
  }

  return internal_empty_discovery_set_;
}

LIBATAPP_MACRO_API uint32_t app::get_address_type(const std::string &address) const noexcept {
  uint32_t ret = address_type_t::EN_ACAT_NONE;

  atbus::channel::channel_address_t addr;
  atbus::channel::make_address(address.c_str(), addr);
  std::transform(addr.scheme.begin(), addr.scheme.end(), addr.scheme.begin(), ::util::string::tolower<char>);

  connector_protocol_map_t::mapped_type connector_protocol = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock_guard{connectors_lock_};
    connector_protocol_map_t::const_iterator iter = connector_protocols_.find(addr.scheme);
    if (iter == connector_protocols_.end()) {
      return ret;
    }

    if (!iter->second) {
      return ret;
    }

    connector_protocol = iter->second;
  }

  return connector_protocol->get_address_type(addr);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t app::get_discovery_node_by_id(uint64_t id) const noexcept {
  if (!internal_module_etcd_) {
    return nullptr;
  }

  return internal_module_etcd_->get_global_discovery().get_node_by_id(id);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t app::get_discovery_node_by_name(const std::string &name) const noexcept {
  if (!internal_module_etcd_) {
    return nullptr;
  }

  return internal_module_etcd_->get_global_discovery().get_node_by_name(name);
}

LIBATAPP_MACRO_API void app::produce_tick_timer_compensation(std::chrono::system_clock::duration tick_cost) noexcept {
  if (tick_cost <= conf_.timer_reserve_interval_tick) {
    tick_timer_.tick_compensation += conf_.timer_reserve_interval_min;
    return;
  }

  int64_t reserve_permille = conf_.timer_reserve_permille;
  if (reserve_permille < 0) {
    reserve_permille = 1;
  } else if (reserve_permille >= 1000) {
    reserve_permille = 999;
  }

  tick_timer_.tick_compensation += std::chrono::system_clock::duration{
      (tick_cost - conf_.timer_reserve_interval_tick).count() * reserve_permille / (1000 - reserve_permille)};
}

LIBATAPP_MACRO_API uint64_t app::consume_tick_timer_compensation() noexcept {
  // This interval will be rounded up to the system clock granularity
  if (tick_clock_granularity_ <= std::chrono::milliseconds::zero()) {
    tick_clock_granularity_ = get_default_system_clock_granularity();
  }

  if (tick_timer_.tick_compensation > tick_clock_granularity_) {
    auto ret = std::chrono::duration_cast<std::chrono::milliseconds>(tick_timer_.tick_compensation).count();
    if UTIL_UNLIKELY_CONDITION (tick_timer_.tick_compensation > conf_.timer_reserve_interval_max) {
      tick_timer_.tick_compensation = std::chrono::system_clock::duration::zero();
      ret = std::chrono::duration_cast<std::chrono::milliseconds>(conf_.timer_reserve_interval_max).count();
    } else {
      tick_timer_.tick_compensation -= std::chrono::milliseconds{ret};
    }
    return static_cast<uint64_t>(ret);
  }

  return 0;
}

LIBATAPP_MACRO_API int32_t app::listen(const std::string &address) {
  atbus::channel::channel_address_t addr;
  atbus::channel::make_address(address.c_str(), addr);
  std::transform(addr.scheme.begin(), addr.scheme.end(), addr.scheme.begin(), ::util::string::tolower<char>);

  connector_protocol_map_t::mapped_type connector_protocol = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(connectors_lock_);
    connector_protocol_map_t::const_iterator iter = connector_protocols_.find(addr.scheme);
    if (iter == connector_protocols_.end()) {
      return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
    }

    if (!iter->second) {
      return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
    }
    connector_protocol = iter->second;
  }

  return connector_protocol->on_start_listen(addr);
}

LIBATAPP_MACRO_API int32_t app::send_message(uint64_t target_node_id, int32_t type, const void *data, size_t data_size,
                                             uint64_t *msg_sequence, const atapp::protocol::atapp_metadata *metadata) {
  if (!check_flag(flag_t::INITIALIZED)) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  // Find from cache
  do {
    atapp_endpoint *cache = get_endpoint(target_node_id);
    if (nullptr == cache && target_node_id == get_app_id()) {
      cache = auto_mutable_self_endpoint().get();
    }

    if (nullptr == cache) {
      break;
    }

    int32_t ret;
    if (nullptr != msg_sequence) {
      ret = cache->push_forward_message(type, *msg_sequence, data, data_size, metadata);
    } else {
      uint64_t msg_seq = 0;
      ret = cache->push_forward_message(type, msg_seq, data, data_size, metadata);
    }
    return ret;
  } while (false);

  // Try to create endpoint from discovery
  do {
    if (!internal_module_etcd_) {
      break;
    }

    etcd_discovery_node::ptr_t node = internal_module_etcd_->get_global_discovery().get_node_by_id(target_node_id);
    if (!node) {
      break;
    }

    return send_message(node, type, data, data_size, msg_sequence, metadata);
  } while (false);

  // Fallback to old atbus connector
  if (check_flag(flag_t::DISABLE_ATBUS_FALLBACK)) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }
  if (!bus_node_) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  return bus_node_->send_data(target_node_id, type, data, data_size, msg_sequence);
}

LIBATAPP_MACRO_API int32_t app::send_message(const std::string &target_node_name, int32_t type, const void *data,
                                             size_t data_size, uint64_t *msg_sequence,
                                             const atapp::protocol::atapp_metadata *metadata) {
  if (!check_flag(flag_t::INITIALIZED)) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  do {
    atapp_endpoint *cache = get_endpoint(target_node_name);
    if (nullptr == cache && target_node_name == get_app_name()) {
      cache = auto_mutable_self_endpoint().get();
    }

    if (nullptr == cache) {
      break;
    }

    int32_t ret;
    if (nullptr != msg_sequence) {
      ret = cache->push_forward_message(type, *msg_sequence, data, data_size, metadata);
    } else {
      uint64_t msg_seq = 0;
      ret = cache->push_forward_message(type, msg_seq, data, data_size, metadata);
    }
    return ret;
  } while (false);

  // Try to create endpoint from discovery
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  etcd_discovery_node::ptr_t node = internal_module_etcd_->get_global_discovery().get_node_by_name(target_node_name);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message(const etcd_discovery_node::ptr_t &target_node_discovery, int32_t type,
                                             const void *data, size_t data_size, uint64_t *msg_sequence,
                                             const atapp::protocol::atapp_metadata *metadata) {
  if (!check_flag(flag_t::INITIALIZED)) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  if (!target_node_discovery) {
    return EN_ATBUS_ERR_PARAMS;
  }

  atapp_endpoint::ptr_t cache = mutable_endpoint(target_node_discovery);
  if (!cache) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  int32_t ret;
  if (nullptr != msg_sequence) {
    ret = cache->push_forward_message(type, *msg_sequence, data, data_size, metadata);
  } else {
    uint64_t msg_seq = 0;
    ret = cache->push_forward_message(type, msg_seq, data, data_size, metadata);
  }
  return ret;
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const void *hash_buf, size_t hash_bufsz, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(internal_module_etcd_->get_global_discovery(), hash_buf, hash_bufsz, type,
                                         data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(uint64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(internal_module_etcd_->get_global_discovery(), hash_key, type, data, data_size,
                                         msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(int64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(internal_module_etcd_->get_global_discovery(), hash_key, type, data, data_size,
                                         msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const std::string &hash_key, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_consistent_hash(internal_module_etcd_->get_global_discovery(), hash_key, type, data, data_size,
                                         msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_random(int32_t type, const void *data, size_t data_size,
                                                       uint64_t *msg_sequence,
                                                       const atapp::protocol::atapp_metadata *metadata) {
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_random(internal_module_etcd_->get_global_discovery(), type, data, data_size, msg_sequence,
                                metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_round_robin(int32_t type, const void *data, size_t data_size,
                                                            uint64_t *msg_sequence,
                                                            const atapp::protocol::atapp_metadata *metadata) {
  if (!internal_module_etcd_) {
    return EN_ATAPP_ERR_DISCOVERY_DISABLED;
  }

  return send_message_by_round_robin(internal_module_etcd_->get_global_discovery(), type, data, data_size, msg_sequence,
                                     metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                const void *hash_buf, size_t hash_bufsz, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_buf, hash_bufsz, metadata);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                uint64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_key, metadata);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                int64_t hash_key, int32_t type, const void *data,
                                                                size_t data_size, uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_key, metadata);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_consistent_hash(const etcd_discovery_set &discovery_set,
                                                                const std::string &hash_key, int32_t type,
                                                                const void *data, size_t data_size,
                                                                uint64_t *msg_sequence,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_consistent_hash(hash_key, metadata);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_random(const etcd_discovery_set &discovery_set, int32_t type,
                                                       const void *data, size_t data_size, uint64_t *msg_sequence,
                                                       const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_random(metadata);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API int32_t app::send_message_by_round_robin(const etcd_discovery_set &discovery_set, int32_t type,
                                                            const void *data, size_t data_size, uint64_t *msg_sequence,
                                                            const atapp::protocol::atapp_metadata *metadata) {
  etcd_discovery_node::ptr_t node = discovery_set.get_node_by_round_robin(metadata);
  if (!node) {
    return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
  }

  return send_message(node, type, data, data_size, msg_sequence, metadata);
}

LIBATAPP_MACRO_API bool app::add_log_sink_maker(gsl::string_view name, log_sink_maker::log_reg_t fn) {
  std::string key{name.data(), name.size()};
  if (log_reg_.end() != log_reg_.find(key)) {
    return false;
  }

  log_reg_[key] = fn;
  return true;
}

LIBATAPP_MACRO_API void app::set_evt_on_forward_request(callback_fn_on_forward_request_t fn) {
  evt_on_forward_request_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_forward_response(callback_fn_on_forward_response_t fn) {
  evt_on_forward_response_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_app_connected(callback_fn_on_connected_t fn) { evt_on_app_connected_ = fn; }

LIBATAPP_MACRO_API void app::set_evt_on_app_disconnected(callback_fn_on_disconnected_t fn) {
  evt_on_app_disconnected_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_all_module_inited(callback_fn_on_all_module_inited_t fn) {
  evt_on_all_module_inited_ = fn;
}
LIBATAPP_MACRO_API void app::set_evt_on_all_module_cleaned(callback_fn_on_all_module_cleaned_t fn) {
  evt_on_all_module_cleaned_ = fn;
}
LIBATAPP_MACRO_API app::callback_fn_on_finally_handle app::add_evt_on_finally(callback_fn_on_finally_t fn) {
  if (!fn) {
    return evt_on_finally_.end();
  }

  return evt_on_finally_.emplace(evt_on_finally_.end(), std::move(fn));
}

LIBATAPP_MACRO_API void app::remove_evt_on_finally(callback_fn_on_finally_handle &handle) {
  if (handle == evt_on_finally_.end()) {
    return;
  }

  evt_on_finally_.erase(handle);
  handle = evt_on_finally_.end();
}

LIBATAPP_MACRO_API void app::clear_evt_on_finally() { evt_on_finally_.clear(); }

LIBATAPP_MACRO_API const app::callback_fn_on_forward_request_t &app::get_evt_on_forward_request() const noexcept {
  return evt_on_forward_request_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_forward_response_t &app::get_evt_on_forward_response() const noexcept {
  return evt_on_forward_response_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_connected_t &app::get_evt_on_app_connected() const noexcept {
  return evt_on_app_connected_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_disconnected_t &app::get_evt_on_app_disconnected() const noexcept {
  return evt_on_app_disconnected_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_all_module_inited_t &app::get_evt_on_all_module_inited() const noexcept {
  return evt_on_all_module_inited_;
}
LIBATAPP_MACRO_API const app::callback_fn_on_all_module_cleaned_t &app::get_evt_on_all_module_cleaned() const noexcept {
  return evt_on_all_module_cleaned_;
}

LIBATAPP_MACRO_API bool app::add_endpoint_waker(util::time::time_utility::raw_time_t wakeup_time,
                                                const atapp_endpoint::weak_ptr_t &ep_watcher,
                                                util::time::time_utility::raw_time_t previous_time) {
  if (is_closing()) {
    return false;
  }

  atapp_endpoint *ep_ptr = ep_watcher.lock().get();
  endpoint_waker_[std::pair<util::time::time_utility::raw_time_t, atapp_endpoint *>(wakeup_time, ep_ptr)] = ep_watcher;
  if (previous_time > wakeup_time) {
    endpoint_waker_.erase(std::pair<util::time::time_utility::raw_time_t, atapp_endpoint *>(previous_time, ep_ptr));
  }
  return true;
}

LIBATAPP_MACRO_API void app::remove_endpoint(uint64_t by_id) {
  endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(by_id);
  if (iter_id == endpoint_index_by_id_.end()) {
    return;
  }

  atapp_endpoint::ptr_t res = iter_id->second;
  endpoint_index_by_id_.erase(iter_id);

  const std::string *name = nullptr;
  if (res) {
    name = &res->get_name();
  }
  if (nullptr != name && !name->empty()) {
    endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(*name);
    if (iter_name != endpoint_index_by_name_.end() && iter_name->second == res) {
      endpoint_index_by_name_.erase(iter_name);
    }
  }

  if (res) {
    atapp_endpoint::internal_accessor::close(*res);
  }
  // RAII destruction of res
}

LIBATAPP_MACRO_API void app::remove_endpoint(const std::string &by_name) {
  endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(by_name);
  if (iter_name == endpoint_index_by_name_.end()) {
    return;
  }

  atapp_endpoint::ptr_t res = iter_name->second;
  endpoint_index_by_name_.erase(iter_name);

  uint64_t id = 0;
  if (res) {
    id = res->get_id();
  }
  if (id != 0) {
    endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(id);
    if (iter_id != endpoint_index_by_id_.end() && iter_id->second == res) {
      endpoint_index_by_id_.erase(iter_id);
    }
  }

  if (res) {
    atapp_endpoint::internal_accessor::close(*res);
  }
  // RAII destruction of res
}

LIBATAPP_MACRO_API void app::remove_endpoint(const atapp_endpoint::ptr_t &enpoint) {
  if (!enpoint) {
    return;
  }

  {
    uint64_t id = enpoint->get_id();
    if (id != 0) {
      endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(id);
      if (iter_id != endpoint_index_by_id_.end() && iter_id->second == enpoint) {
        endpoint_index_by_id_.erase(iter_id);
      }
    }
  }

  {
    const std::string &name = enpoint->get_name();
    if (!name.empty()) {
      endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(name);
      if (iter_name != endpoint_index_by_name_.end() && iter_name->second == enpoint) {
        endpoint_index_by_name_.erase(iter_name);
      }
    }
  }

  if (enpoint) {
    atapp_endpoint::internal_accessor::close(*enpoint);
  }
}

LIBATAPP_MACRO_API atapp_endpoint::ptr_t app::mutable_endpoint(const etcd_discovery_node::ptr_t &discovery) {
  if (is_closing()) {
    return nullptr;
  }

  if (!discovery) {
    return nullptr;
  }

  uint64_t id = discovery->get_discovery_info().id();
  const std::string &name = discovery->get_discovery_info().name();
  atapp_endpoint::ptr_t ret;
  bool is_created = false;
  bool need_update_id_index = false;
  bool need_update_name_index = false;
  do {
    if (0 == id) {
      break;
    }

    endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(id);
    if (iter_id != endpoint_index_by_id_.end()) {
      ret = iter_id->second;
    }

    need_update_id_index = !ret;
  } while (false);

  do {
    if (name.empty()) {
      break;
    }

    endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(name);
    if (iter_name != endpoint_index_by_name_.end()) {
      if (ret && iter_name->second == ret) {
        break;
      }
      if (ret) {
        remove_endpoint(id);
        need_update_id_index = true;
      }
      ret = iter_name->second;
      need_update_name_index = !ret;
    } else {
      need_update_name_index = true;
    }
  } while (false);

  if (!ret) {
    ret = atapp_endpoint::create(*this);
    is_created = !!ret;
  }
  if (ret) {
    if (need_update_id_index) {
      endpoint_index_by_id_[id] = ret;
    }
    if (need_update_name_index) {
      endpoint_index_by_name_[name] = ret;
    }
    ret->update_discovery(discovery);
  }

  // Wake and maybe it's should be cleanup if it's a new endpoint
  if (is_created && ret) {
    ret->add_waker(get_last_tick_time());
    atapp_connection_handle::ptr_t handle = std::make_shared<atapp_connection_handle>();

    bool is_loopback = (0 == id || id == get_app_id()) && (name.empty() || name == get_app_name());

    // Check loopback
    if (handle.use_count() == 1 && loopback_connector_ && is_loopback) {
      atbus::channel::channel_address_t addr;
      atbus::channel::make_address("loopback://", get_app_name().c_str(), 0, addr);
      int res = loopback_connector_->on_start_connect(discovery.get(), addr, handle);
      if (0 == res && handle.use_count() > 1) {
        atapp_connector_bind_helper::bind(*handle, *loopback_connector_);
        atapp_endpoint_bind_helper::bind(*handle, *ret);

        FWLOGINFO("atapp endpoint {}({}) connect loopback connector success and use handle {}", ret->get_id(),
                  ret->get_name(), addr.address, reinterpret_cast<const void *>(handle.get()));
      } else {
        FWLOGINFO("atapp endpoint {}({}) skip loopback connector with handle {}, connect result {}", ret->get_id(),
                  ret->get_name(), addr.address, reinterpret_cast<const void *>(handle.get()), res);
      }
    } else {
      int32_t gateway_size = discovery->get_ingress_size();
      for (int32_t i = 0; handle && i < gateway_size; ++i) {
        atbus::channel::channel_address_t addr;
        const atapp::protocol::atapp_gateway &gateway = discovery->next_ingress_gateway();
        if (!match_gateway(gateway)) {
          FWLOGDEBUG("atapp endpoint {}({}) skip unmatched gateway {}", ret->get_id(), ret->get_name(),
                     gateway.address());
          continue;
        }
        atbus::channel::make_address(gateway.address().c_str(), addr);
        std::transform(addr.scheme.begin(), addr.scheme.end(), addr.scheme.begin(), ::util::string::tolower<char>);

        connector_protocol_map_t::mapped_type connector_protocol = nullptr;
        {
          std::lock_guard<std::recursive_mutex> lock_guard{connectors_lock_};
          connector_protocol_map_t::const_iterator iter = connector_protocols_.find(addr.scheme);
          if (iter == connector_protocols_.end()) {
            FWLOGDEBUG("atapp endpoint {}({}) skip unsupported address {}", ret->get_id(), ret->get_name(),
                       addr.address);
            continue;
          }

          if (!iter->second) {
            FWLOGDEBUG("atapp endpoint {}({}) skip unsupported address {}", ret->get_id(), ret->get_name(),
                       addr.address);
            continue;
          }
          connector_protocol = iter->second;
        }

        // Skip loopback and use inner loopback connector
        if (is_loopback && !connector_protocol->support_loopback()) {
          continue;
        }

        int res = connector_protocol->on_start_connect(discovery.get(), addr, handle);
        if (0 == res && handle.use_count() > 1) {
          atapp_connector_bind_helper::bind(*handle, *connector_protocol);
          atapp_endpoint_bind_helper::bind(*handle, *ret);

          FWLOGINFO("connect address {} of atapp endpoint {}({}) success and use handle {}", ret->get_id(),
                    ret->get_name(), addr.address, reinterpret_cast<const void *>(handle.get()));
          break;
        } else {
          FWLOGINFO("skip address {} of atapp endpoint {}({}) with handle {}, connect result {}", ret->get_id(),
                    ret->get_name(), addr.address, reinterpret_cast<const void *>(handle.get()), res);
        }
      }
    }
  }

  return ret;
}

LIBATAPP_MACRO_API atapp_endpoint *app::get_endpoint(uint64_t by_id) {
  endpoint_index_by_id_t::iterator iter_id = endpoint_index_by_id_.find(by_id);
  if (iter_id != endpoint_index_by_id_.end()) {
    return iter_id->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API const atapp_endpoint *app::get_endpoint(uint64_t by_id) const noexcept {
  endpoint_index_by_id_t::const_iterator iter_id = endpoint_index_by_id_.find(by_id);
  if (iter_id != endpoint_index_by_id_.end()) {
    return iter_id->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API atapp_endpoint *app::get_endpoint(const std::string &by_name) {
  endpoint_index_by_name_t::iterator iter_name = endpoint_index_by_name_.find(by_name);
  if (iter_name != endpoint_index_by_name_.end()) {
    return iter_name->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API const atapp_endpoint *app::get_endpoint(const std::string &by_name) const noexcept {
  endpoint_index_by_name_t::const_iterator iter_name = endpoint_index_by_name_.find(by_name);
  if (iter_name != endpoint_index_by_name_.end()) {
    return iter_name->second.get();
  }

  return nullptr;
}

LIBATAPP_MACRO_API bool app::match_gateway(const atapp::protocol::atapp_gateway &checked) const noexcept {
  if (checked.address().empty()) {
    return false;
  }

  if (checked.match_hosts_size() > 0 && !match_gateway_hosts(checked)) {
    return false;
  }

  if (checked.match_namespaces_size() && !match_gateway_namespace(checked)) {
    return false;
  }

  if (checked.match_labels_size() > 0 && !match_gateway_labels(checked)) {
    return false;
  }

  return true;
}

LIBATAPP_MACRO_API void app::setup_logger(util::log::log_wrapper &logger, const std::string &min_level,
                                          const atapp::protocol::atapp_log_category &log_conf) const noexcept {
  int log_level_id = util::log::log_wrapper::level_t::LOG_LW_INFO;
  log_level_id = util::log::log_formatter::get_level_by_name(min_level.c_str());

  // init and set prefix
  if (0 != logger.init(WLOG_LEVELID(log_level_id))) {
    FWLOGERROR("Log initialize for {}({}) failed, skipped", log_conf.name(), log_conf.index());
    return;
  }

  if (!log_conf.prefix().empty()) {
    logger.set_prefix_format(log_conf.prefix());
  }

  // load stacktrace configure
  if (!log_conf.stacktrace().min().empty() || !log_conf.stacktrace().max().empty()) {
    util::log::log_formatter::level_t::type stacktrace_level_min = util::log::log_formatter::level_t::LOG_LW_DISABLED;
    util::log::log_formatter::level_t::type stacktrace_level_max = util::log::log_formatter::level_t::LOG_LW_DISABLED;
    if (!log_conf.stacktrace().min().empty()) {
      stacktrace_level_min = util::log::log_formatter::get_level_by_name(log_conf.stacktrace().min().c_str());
    }

    if (!log_conf.stacktrace().max().empty()) {
      stacktrace_level_max = util::log::log_formatter::get_level_by_name(log_conf.stacktrace().max().c_str());
    }

    logger.set_stacktrace_level(stacktrace_level_max, stacktrace_level_min);
  }

  // For now, only log level can be reload
  size_t old_sink_number = logger.sink_size();
  size_t new_sink_number = 0;

  // register log handles
  for (int j = 0; j < log_conf.sink_size(); ++j) {
    const ::atapp::protocol::atapp_log_sink &log_sink = log_conf.sink(j);
    int log_handle_min = util::log::log_wrapper::level_t::LOG_LW_FATAL,
        log_handle_max = util::log::log_wrapper::level_t::LOG_LW_DEBUG;
    if (!log_sink.level().min().empty()) {
      log_handle_min = util::log::log_formatter::get_level_by_name(log_sink.level().min().c_str());
    }
    if (!log_sink.level().max().empty()) {
      log_handle_max = util::log::log_formatter::get_level_by_name(log_sink.level().max().c_str());
    }

    // register log sink
    if (new_sink_number >= old_sink_number) {
      std::unordered_map<std::string, log_sink_maker::log_reg_t>::const_iterator iter = log_reg_.find(log_sink.type());
      if (iter != log_reg_.end()) {
        util::log::log_wrapper::log_handler_t log_handler = iter->second(logger, j, conf_.log, log_conf, log_sink);
        logger.add_sink(log_handler, static_cast<util::log::log_wrapper::level_t::type>(log_handle_min),
                        static_cast<util::log::log_wrapper::level_t::type>(log_handle_max));
        ++new_sink_number;
      } else {
        FWLOGERROR("Unavailable log type {} for log {}({}), you can add log type register handle before init.",
                   log_sink.type(), log_conf.name(), log_conf.index());
      }
    } else {
      logger.set_sink(new_sink_number, static_cast<util::log::log_wrapper::level_t::type>(log_handle_min),
                      static_cast<util::log::log_wrapper::level_t::type>(log_handle_max));
      ++new_sink_number;
    }
  }

  while (logger.sink_size() > new_sink_number) {
    logger.pop_sink();
  }
}

void app::ev_stop_timeout(uv_timer_t *handle) {
  assert(handle && handle->data);

  if (nullptr != handle && nullptr != handle->data) {
    app *self = reinterpret_cast<app *>(handle->data);
    self->set_flag(flag_t::TIMEOUT, true);
  }

  if (nullptr != handle) {
    uv_stop(handle->loop);
  }
}

bool app::set_flag(flag_t::type f, bool v) {
  if (f < 0 || f >= flag_t::FLAG_MAX) {
    return false;
  }

  uint64_t mask_value = static_cast<uint64_t>(1) << static_cast<uint32_t>(f);

  uint64_t current_value = flags_.load(std::memory_order_acquire);
  while (true) {
    uint64_t expect_value = v ? (current_value | mask_value) : (current_value & (~mask_value));
    if (current_value == expect_value) {
      return v;
    }

    if (flags_.compare_exchange_weak(current_value, expect_value, std::memory_order_acq_rel)) {
      return !v;
    }
  }

  return 0 != (current_value & mask_value);
}

LIBATAPP_MACRO_API bool app::check_flag(flag_t::type f) const noexcept {
  if (f < 0 || f >= flag_t::FLAG_MAX) {
    return false;
  }

  return 0 != (flags_.load(std::memory_order_acquire) & (static_cast<uint64_t>(1) << static_cast<uint32_t>(f)));
}

int app::apply_configure() {
  std::string old_name = conf_.origin.name();
  std::string old_hostname = conf_.origin.hostname();
  std::string old_identity = conf_.origin.identity();

  conf_.previous_origin.Clear();
  conf_.previous_origin.Swap(&conf_.origin);

  parse_configures_into(conf_.origin, "atapp", "ATAPP");

  // id and id mask
  if (conf_.id_mask.empty()) {
    split_ids_by_string(conf_.origin.id_mask().c_str(), conf_.id_mask);
  }

  if (!conf_.id_cmd.empty()) {
    conf_.id = convert_app_id_by_string(conf_.id_cmd.c_str());
  }

  if (0 == conf_.id) {
    conf_.id = convert_app_id_by_string(conf_.origin.id().c_str());
  }

  // Changing name is not allowed
  if (!old_name.empty()) {
    conf_.origin.set_name(old_name);
  } else if (conf_.origin.name().empty()) {
    conf_.origin.set_name(LOG_WRAPPER_FWAPI_FORMAT("{}-0x{:x}", conf_.origin.type_name(), conf_.id));
  }

  {
    uint64_t hash_out[2];
    ::util::hash::murmur_hash3_x64_128(conf_.origin.name().c_str(), static_cast<int>(conf_.origin.name().size()),
                                       LIBATAPP_MACRO_HASH_MAGIC_NUMBER, hash_out);
    conf_.hash_code = LOG_WRAPPER_FWAPI_FORMAT("{:016X}{:016X}", hash_out[0], hash_out[1]);
  }

  // Changing hostname is not allowed
  if (!old_hostname.empty()) {
    conf_.origin.set_hostname(old_hostname);
  }

  if (!conf_.origin.hostname().empty()) {
    atbus::node::set_hostname(conf_.origin.hostname());
  }

  // Changing identity is not allowed
  if (!old_identity.empty()) {
    conf_.origin.set_identity(old_identity);
  }
  if (conf_.origin.identity().empty()) {
    std::stringstream identity_stream;
    identity_stream << util::file_system::get_abs_path(conf_.execute_path) << std::endl;
    identity_stream << util::file_system::get_abs_path(conf_.conf_file.c_str()) << std::endl;
    identity_stream << "id: " << conf_.id << std::endl;
    identity_stream << "name: " << conf_.origin.name() << std::endl;
    identity_stream << "hostname: " << conf_.origin.hostname() << std::endl;
    std::string identity_buffer = identity_stream.str();
    conf_.origin.set_identity(util::hash::sha::hash_to_hex(util::hash::sha::EN_ALGORITHM_SHA256,
                                                           identity_buffer.c_str(), identity_buffer.size()));
  }

  // reset metadata from configure
  if (conf_.origin.has_metadata()) {
    conf_.metadata = conf_.origin.metadata();
  }

  if (conf_.origin.has_runtime()) {
    mutable_runtime_configure() = conf_.origin.runtime();
    conf_.runtime_pod_stateful_index = static_cast<int32_t>(atapp_pod_stateful_index::kUnset);
  }

  // timer configure
  {
    conf_.timer_tick_interval = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::seconds(conf_.origin.timer().tick_interval().seconds()));
    conf_.timer_tick_interval += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(conf_.origin.timer().tick_interval().nanos()));

    if (conf_.timer_tick_interval < std::chrono::milliseconds(1)) {
      conf_.timer_tick_interval =
          std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(8));
    }

    conf_.timer_tick_round_timeout = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::seconds(conf_.origin.timer().tick_round_timeout().seconds()));
    conf_.timer_tick_round_timeout += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(conf_.origin.timer().tick_round_timeout().nanos()));

    if (conf_.timer_tick_round_timeout < conf_.timer_tick_interval) {
      conf_.timer_tick_round_timeout = conf_.timer_tick_interval * 8;
    }

    conf_.timer_reserve_permille = conf_.origin.timer().reserve_permille();
    if (conf_.timer_reserve_permille <= 0) {
      conf_.timer_reserve_permille = 10;
    } else if (conf_.timer_reserve_permille >= 1000) {
      conf_.timer_reserve_permille = 999;
    }

    conf_.timer_reserve_interval_min = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::seconds(conf_.origin.timer().reserve_interval_min().seconds()));
    conf_.timer_reserve_interval_min += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(conf_.origin.timer().reserve_interval_min().nanos()));
    if (conf_.timer_reserve_interval_min < std::chrono::microseconds(1)) {
      conf_.timer_reserve_interval_min =
          std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::microseconds(100));
    }

    conf_.timer_reserve_interval_max = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::seconds(conf_.origin.timer().reserve_interval_max().seconds()));
    conf_.timer_reserve_interval_max += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(conf_.origin.timer().reserve_interval_max().nanos()));
    if (conf_.timer_reserve_interval_max < std::chrono::milliseconds(1)) {
      conf_.timer_reserve_interval_max =
          std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(1));
    }
    if (conf_.timer_reserve_interval_max < conf_.timer_reserve_interval_min) {
      conf_.timer_reserve_interval_max = conf_.timer_reserve_interval_min;
    }

    conf_.timer_reserve_interval_tick = std::chrono::system_clock::duration{
        conf_.timer_tick_interval.count() * (1000 - conf_.timer_reserve_permille) / 1000};
    {
      std::chrono::system_clock::duration reserve_interval_tick_from_min =
          std::chrono::system_clock::duration{conf_.timer_reserve_interval_min.count() *
                                              (1000 - conf_.timer_reserve_permille) / conf_.timer_reserve_permille};
      if (reserve_interval_tick_from_min > conf_.timer_reserve_interval_tick) {
        conf_.timer_reserve_interval_tick = reserve_interval_tick_from_min;
      }
    }
  }

  // atbus configure
  atbus::node::default_conf(&conf_.bus_conf);

  {
    conf_.bus_conf.subnets.reserve(static_cast<size_t>(conf_.origin.bus().subnets_size()));
    for (int i = 0; i < conf_.origin.bus().subnets_size(); ++i) {
      const std::string &subset = conf_.origin.bus().subnets(i);
      std::string::size_type sep_pos = subset.find('/');
      if (std::string::npos == sep_pos) {
        conf_.bus_conf.subnets.push_back(
            atbus::endpoint_subnet_conf(0, util::string::to_int<uint32_t>(subset.c_str())));
      } else {
        conf_.bus_conf.subnets.push_back(atbus::endpoint_subnet_conf(
            convert_app_id_by_string(subset.c_str()), util::string::to_int<uint32_t>(subset.c_str() + sep_pos + 1)));
      }
    }
  }

  conf_.bus_conf.parent_address = conf_.origin.bus().proxy();
  conf_.bus_conf.loop_times = conf_.origin.bus().loop_times();
  conf_.bus_conf.ttl = conf_.origin.bus().ttl();
  conf_.bus_conf.backlog = conf_.origin.bus().backlog();
  conf_.bus_conf.access_token_max_number = static_cast<size_t>(conf_.origin.bus().access_token_max_number());
  {
    conf_.bus_conf.access_tokens.reserve(static_cast<size_t>(conf_.origin.bus().access_tokens_size()));
    for (int i = 0; i < conf_.origin.bus().access_tokens_size(); ++i) {
      const std::string &access_token = conf_.origin.bus().access_tokens(i);
      conf_.bus_conf.access_tokens.push_back(std::vector<unsigned char>());
      conf_.bus_conf.access_tokens.back().assign(
          reinterpret_cast<const unsigned char *>(access_token.data()),
          reinterpret_cast<const unsigned char *>(access_token.data()) + access_token.size());
    }
  }
  conf_.bus_conf.overwrite_listen_path = conf_.origin.bus().overwrite_listen_path();

  conf_.bus_conf.first_idle_timeout = conf_.origin.bus().first_idle_timeout().seconds();
  conf_.bus_conf.ping_interval = conf_.origin.bus().ping_interval().seconds();
  conf_.bus_conf.retry_interval = conf_.origin.bus().retry_interval().seconds();

  conf_.bus_conf.fault_tolerant = static_cast<size_t>(conf_.origin.bus().fault_tolerant());
  conf_.bus_conf.msg_size = static_cast<size_t>(conf_.origin.bus().msg_size());
  conf_.bus_conf.recv_buffer_size = static_cast<size_t>(conf_.origin.bus().recv_buffer_size());
  conf_.bus_conf.send_buffer_size = static_cast<size_t>(conf_.origin.bus().send_buffer_size());
  conf_.bus_conf.send_buffer_number = static_cast<size_t>(conf_.origin.bus().send_buffer_number());

  return 0;
}  // namespace atapp

void app::run_ev_loop(int run_mode) {
  util::time::time_utility::update();

  ev_loop_t *loop = get_evloop();
  if (bus_node_) {
    assert(loop);
    // step X. loop uv_run util stop flag is set
    if (nullptr != loop) {
      flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
      uv_run(loop, static_cast<uv_run_mode>(run_mode));
    }

    if (0 != pending_signals_[0]) {
      process_signals();
    }

    if (check_flag(flag_t::STOPING)) {
      set_flag(flag_t::STOPPED, true);

      if (check_flag(flag_t::TIMEOUT)) {
        // step X. notify all modules timeout
        for (module_ptr_t &mod : modules_) {
          if (mod->is_enabled()) {
            FWLOGERROR("try to stop module {} but timeout", mod->name());
            mod->timeout();
            mod->disable();
          }
        }
      } else {
        // step X. notify all modules to finish and wait for all modules stop
        for (module_ptr_t &mod : modules_) {
          if (!mod->is_enabled()) {
            continue;
          }

          if (mod->check_suspend_stop()) {
            // any module stop running will make app wait
            set_flag(flag_t::STOPPED, false);
            continue;
          }

          int res = mod->stop();
          if (0 == res) {
            mod->disable();
          } else if (res < 0) {
            mod->disable();
            FWLOGERROR("try to stop module {} but failed and return {}", mod->name(), res);
          } else {
            // any module stop running will make app wait
            set_flag(flag_t::STOPPED, false);
          }
        }

        // step X. if stop is blocked and timeout not triggered, setup stop timeout and waiting for all modules finished
        if (!tick_timer_.timeout_timer && nullptr != loop) {
          setup_timeout_timer(conf_.origin.timer().stop_timeout());
        }
      }

      // stop atbus after all module closed
      if (check_flag(flag_t::STOPPED) && bus_node_ && ::atbus::node::state_t::CREATED != bus_node_->get_state() &&
          !bus_node_->check_flag(::atbus::node::flag_t::EN_FT_SHUTDOWN)) {
        bus_node_->shutdown(0);
      }
    }

    // if atbus is at shutdown state, loop event dispatcher using next while iterator
  }
}

int app::run_inner(int run_mode) {
  if (!check_flag(flag_t::INITIALIZED) && !check_flag(flag_t::INITIALIZING)) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  ev_loop_t *loop = get_evloop();
  if (nullptr == loop) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  stats_.last_proc_event_count = 0;
  if (check_flag(flag_t::IN_CALLBACK)) {
    return 0;
  }

  if (mode_t::START != mode_) {
    return 0;
  }

  run_ev_loop(run_mode);

  if (is_closed() && is_inited()) {
    // close timer
    close_timer(tick_timer_.tick_timer);
    close_timer(tick_timer_.timeout_timer);

    // cleanup modules
    for (std::vector<module_ptr_t>::reverse_iterator rit = modules_.rbegin(); rit != modules_.rend(); ++rit) {
      if (*rit) {
        (*rit)->cleanup();
      }
    }

    if (evt_on_all_module_cleaned_) {
      evt_on_all_module_cleaned_(*this);
    }

    // cleanup pid file
    cleanup_pidfile();

    set_flag(flag_t::INITIALIZED, false);
    set_flag(flag_t::RUNNING, false);

    // finally events
    app_evt_on_finally();
  }

  if (stats_.last_proc_event_count > 0) {
    return 1;
  }

  return 0;
}

void app::_app_setup_signal_handle(int signo) {
  // Signal handle has a limited stack can process signal on next proc to keep signal safety
  app *current = app::last_instance_;
  if (nullptr == current) {
    return;
  }

  for (int i = 0; i < MAX_SIGNAL_COUNT; ++i) {
    if (0 == current->pending_signals_[i]) {
      current->pending_signals_[i] = signo;
      break;
    }
  }

  ev_loop_t *loop = current->get_evloop();
  if (nullptr != loop) {
    uv_stop(loop);
  }
}

void app::process_signals() {
  if (0 == pending_signals_[0]) {
    return;
  }

  int signals[MAX_SIGNAL_COUNT] = {0};
  memcpy(signals, pending_signals_, sizeof(pending_signals_));
  memset(pending_signals_, 0, sizeof(pending_signals_));

  for (int i = 0; i < MAX_SIGNAL_COUNT; ++i) {
    if (0 == signals[i]) {
      break;
    }

    process_signal(signals[i]);
  }
}

void app::process_signal(int signo) {
  switch (signo) {
#ifndef WIN32
    case SIGSTOP:
#endif
    case SIGTERM: {
      conf_.upgrade_mode = false;
      stop();
      break;
    }

    default: {
      break;
    }
  }
}

int32_t app::process_inner_events(const util::time::time_utility::raw_time_t &end_tick) {
  int32_t ret = 0;
  bool more_messages = true;
  bool first_round = true;
  while ((first_round || util::time::time_utility::sys_now() < end_tick) && more_messages) {
    first_round = false;
    int32_t round_res = 0;
    if (!endpoint_waker_.empty() && endpoint_waker_.begin()->first.first <= tick_timer_.last_tick_timepoint) {
      atapp_endpoint::ptr_t ep = endpoint_waker_.begin()->second.lock();
      endpoint_waker_.erase(endpoint_waker_.begin());
      ++stats_.endpoint_wake_count;

      if (ep) {
        FWLOGDEBUG("atapp {:#x}({}) wakeup endpint {}({:#x}, {})", get_app_id(), get_app_name(),
                   reinterpret_cast<const void *>(ep.get()), ep->get_id(), ep->get_name());
        int32_t res = ep->retry_pending_messages(tick_timer_.last_tick_timepoint, conf_.origin.bus().loop_times());
        if (res > 0) {
          round_res += res;
        }

        // TODO(owent): Support for delay recycle?
        if (!ep->has_connection_handle()) {
          remove_endpoint(ep);
        }
      }
    }

    if (loopback_connector_) {
      round_res += loopback_connector_->process(end_tick, conf_.origin.bus().loop_times());
    }

    if (round_res > 0) {
      ret += round_res;
    } else {
      // No more event, exit directly
      more_messages = false;
    }
  }

  return ret;
}

atapp_endpoint::ptr_t app::auto_mutable_self_endpoint() {
  auto self_discovery = util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  if (!self_discovery) {
    return nullptr;
  }

  atapp::protocol::atapp_discovery self_discovery_info;
  pack(self_discovery_info);
  self_discovery->copy_from(self_discovery_info, atapp::etcd_discovery_node::node_version());

  return mutable_endpoint(self_discovery);
}

namespace {
static bool setup_load_sink_from_environment(gsl::string_view prefix, atapp::protocol::atapp_log_sink &out,
                                             configure_key_set *dump_existed_set, gsl::string_view exist_set_prefix) {
  if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_file_sink_name().data())) {
    // Inner file sink
    return environment_loader_dump_to(prefix, *out.mutable_log_backend_file(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stdout_sink_name().data())) {
    // Inner stdout sink
    return environment_loader_dump_to(prefix, *out.mutable_log_backend_stdout(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stderr_sink_name().data())) {
    // Inner stderr sink
    return environment_loader_dump_to(prefix, *out.mutable_log_backend_stderr(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_syslog_sink_name().data())) {
    // Inner syslog sink
    return environment_loader_dump_to(prefix, *out.mutable_log_backend_syslog(), dump_existed_set, exist_set_prefix);
  }

  // We do not load custom log configure from environment right now
  return false;
}

static void setup_load_category_from_environment(
    gsl::string_view prefix,
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atapp_log_category> &out,
    configure_key_set *dump_existed_set, gsl::string_view exist_set_prefix) {
  for (int32_t cat_default_index = 0;; ++cat_default_index) {
    std::string cat_prefix = util::log::format("{}_CATEGORY_{}", prefix, cat_default_index);
    std::transform(cat_prefix.begin(), cat_prefix.end(), cat_prefix.begin(), util::string::toupper<char>);

    std::string cat_name_key = util::log::format("{}_NAME", cat_prefix);
    std::string log_name = util::file_system::getenv(cat_name_key.c_str());
    if (log_name.empty()) {
      break;
    }

    std::string cat_index_key = util::log::format("{}_INDEX", cat_prefix);
    std::string cat_index_val = util::file_system::getenv(cat_index_key.c_str());
    int32_t cat_index;
    if (cat_index_val.empty()) {
      cat_index = cat_default_index;
    } else {
      cat_index = util::string::to_int<int32_t>(cat_index_val.c_str());
    }

    atapp::protocol::atapp_log_category *log_cat = nullptr;
    for (int i = 0; i < out.size(); ++i) {
      if (out.Get(i).name() == log_name || out.Get(i).index() == cat_index) {
        log_cat = out.Mutable(i);
        cat_index = i;
      }
    }

    if (nullptr == log_cat) {
      log_cat = out.Add();
    }

    if (nullptr == log_cat) {
      FWLOGERROR("log {} malloc category failed, skipped.", log_name);
      return;
    }

    std::string exist_set_category_prefix = util::log::format("{}category.{}.", exist_set_prefix, cat_index);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(
          static_cast<std::string>(exist_set_category_prefix.substr(0, exist_set_category_prefix.size() - 1)));
    }

    environment_loader_dump_to(cat_prefix, *log_cat, dump_existed_set, exist_set_category_prefix);
    // overwrite index
    log_cat->set_index(cat_index);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(util::log::format("{}index", exist_set_category_prefix));
    }

    // register log handles
    for (int32_t log_handle_index = 0;; ++log_handle_index) {
      std::string cat_handle_prefix = util::log::format("{}_{}_{}", prefix, log_name, log_handle_index);
      std::transform(cat_handle_prefix.begin(), cat_handle_prefix.end(), cat_handle_prefix.begin(),
                     util::string::toupper<char>);

      std::string cat_handle_type_key = util::log::format("{}_TYPE", cat_handle_prefix, log_handle_index);
      std::string sink_type = util::file_system::getenv(cat_handle_type_key.c_str());
      if (sink_type.empty()) {
        break;
      }

      ::atapp::protocol::atapp_log_sink *log_sink = log_cat->add_sink();
      if (nullptr == log_sink) {
        FWLOGERROR("log {} malloc sink {} (index: {}) failed, skipped.", log_name, sink_type, log_handle_index);
        continue;
      }

      std::string exist_set_sink_prefix = util::log::format("{}sink.{}.", exist_set_category_prefix, log_handle_index);
      if (nullptr != dump_existed_set) {
        dump_existed_set->insert(
            static_cast<std::string>(exist_set_sink_prefix.substr(0, exist_set_sink_prefix.size() - 1)));
      }

      environment_loader_dump_to(cat_handle_prefix, *log_sink, dump_existed_set, exist_set_sink_prefix);
      setup_load_sink_from_environment(cat_handle_prefix, *log_sink, dump_existed_set, exist_set_sink_prefix);
    }
  }
}
}  // namespace

void app::parse_environment_log_categories_into(atapp::protocol::atapp_log &dst,
                                                gsl::string_view load_environemnt_prefix,
                                                configure_key_set *dump_existed_set) const noexcept {
  std::string env_level_name;
  env_level_name.reserve(load_environemnt_prefix.size() + 6);
  env_level_name = static_cast<std::string>(load_environemnt_prefix);
  env_level_name += "_LEVEL";

  std::string level_value = util::file_system::getenv(env_level_name.c_str());
  if (!level_value.empty()) {
    dst.set_level(level_value);

    if (nullptr != dump_existed_set) {
      dump_existed_set->insert("level");
    }
  }

  setup_load_category_from_environment(load_environemnt_prefix, *dst.mutable_category(), dump_existed_set, "");
}

namespace {
static void setup_load_sink(const util::config::ini_value &log_sink_cfg_src, atapp::protocol::atapp_log_sink &out,
                            configure_key_set *dump_existed_set, gsl::string_view exist_set_prefix) {
  // yaml_loader_dump_to(src, out); // already dumped before in setup_load_category(...)

  if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_file_sink_name().data())) {
    // Inner file sink
    ini_loader_dump_to(log_sink_cfg_src, *out.mutable_log_backend_file(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stdout_sink_name().data())) {
    // Inner stdout sink
    ini_loader_dump_to(log_sink_cfg_src, *out.mutable_log_backend_stdout(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stderr_sink_name().data())) {
    // Inner stderr sink
    ini_loader_dump_to(log_sink_cfg_src, *out.mutable_log_backend_stderr(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_syslog_sink_name().data())) {
    // Inner syslog sink
    ini_loader_dump_to(log_sink_cfg_src, *out.mutable_log_backend_syslog(), dump_existed_set, exist_set_prefix);
  } else {
    // Dump all configures into unresolved_key_values
    ini_loader_dump_to(log_sink_cfg_src, *out.mutable_unresolved_key_values(), "", dump_existed_set, exist_set_prefix);
  }
}

static void setup_load_category(
    const util::config::ini_value &src,
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atapp_log_category> &out,
    configure_key_set *dump_existed_set, gsl::string_view exist_set_prefix) {
  auto cat_array_node = src.get_children().find("category");
  // Compatibile with old ini configure files
  if (src.get_children().end() == cat_array_node) {
    cat_array_node = src.get_children().find("cat");
  }
  if (src.get_children().end() == cat_array_node) {
    return;
  }
  if (!cat_array_node->second) {
    return;
  }

  for (int32_t cat_default_index = 0;; ++cat_default_index) {
    auto cat_node = cat_array_node->second->get_children().find(util::log::format("{}", cat_default_index));
    if (cat_node == cat_array_node->second->get_children().end()) {
      break;
    }
    if (!cat_node->second) {
      break;
    }
    if (cat_node->second->empty()) {
      break;
    }

    std::string log_name = (*cat_node->second)["name"].as_cpp_string();
    if (log_name.empty()) {
      continue;
    }

    int32_t cat_index = cat_default_index;
    {
      auto cat_index_node = cat_node->second->get_children().find("index");
      if (cat_node->second->get_children().end() != cat_index_node) {
        cat_index = cat_index_node->second->as_int32();
      }
    }

    atapp::protocol::atapp_log_category *log_cat = nullptr;
    for (int i = 0; i < out.size(); ++i) {
      if (out.Get(i).name() == log_name || out.Get(i).index() == cat_index) {
        log_cat = out.Mutable(i);
        cat_index = i;
      }
    }

    if (nullptr == log_cat) {
      log_cat = out.Add();
    }

    if (nullptr == log_cat) {
      FWLOGERROR("log {} malloc category failed, skipped.", log_name);
      return;
    }

    std::string exist_set_category_prefix = util::log::format("{}category.{}.", exist_set_prefix, cat_index);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(
          static_cast<std::string>(exist_set_category_prefix.substr(0, exist_set_category_prefix.size() - 1)));
    }

    ini_loader_dump_to(*cat_node->second, *log_cat, dump_existed_set, exist_set_category_prefix);
    // overwrite index
    log_cat->set_index(cat_index);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(util::log::format("{}index", exist_set_category_prefix));
    }

    auto cat_handles_node = src.get_children().find(log_name);
    if (cat_handles_node == src.get_children().end()) {
      // No handles
      continue;
    }
    if (!cat_handles_node->second) {
      continue;
    }

    // register log handles
    for (int32_t log_handle_index = 0;; ++log_handle_index) {
      auto cat_handle_node = cat_handles_node->second->get_children().find(util::log::format("{}", log_handle_index));
      if (cat_handle_node == cat_handles_node->second->get_children().end()) {
        break;
      }
      if (!cat_handle_node->second) {
        break;
      }

      std::string sink_type = (*cat_handle_node->second)["type"].as_cpp_string();
      if (sink_type.empty()) {
        break;
      }

      ::atapp::protocol::atapp_log_sink *log_sink = log_cat->add_sink();
      if (nullptr == log_sink) {
        FWLOGERROR("log {} malloc sink {} (index: {}) failed, skipped.", log_name, sink_type, log_handle_index);
        continue;
      }

      std::string exist_set_sink_prefix = util::log::format("{}sink.{}.", exist_set_category_prefix, log_handle_index);
      if (nullptr != dump_existed_set) {
        dump_existed_set->insert(
            static_cast<std::string>(exist_set_sink_prefix.substr(0, exist_set_sink_prefix.size() - 1)));
      }

      ini_loader_dump_to(*cat_handle_node->second, *log_sink, dump_existed_set, exist_set_sink_prefix);
      setup_load_sink(*cat_handle_node->second, *log_sink, dump_existed_set, exist_set_sink_prefix);
    }
  }
}
}  // namespace

void app::parse_ini_log_categories_into(atapp::protocol::atapp_log &dst, const std::vector<gsl::string_view> &path,
                                        configure_key_set *dump_existed_set) const noexcept {
  const util::config::ini_value *log_root_node = nullptr;
  for (auto &key : path) {
    if (nullptr == log_root_node) {
      auto iter = cfg_loader_.get_root_node().get_children().find(static_cast<std::string>(key));
      if (iter == cfg_loader_.get_root_node().get_children().end()) {
        break;
      }
      log_root_node = iter->second.get();
    } else {
      auto iter = log_root_node->get_children().find(static_cast<std::string>(key));
      if (iter == log_root_node->get_children().end()) {
        break;
      }
      log_root_node = iter->second.get();
    }
    if (nullptr == log_root_node) {
      break;
    }
  }

  if (nullptr == log_root_node) {
    return;
  }

  {
    auto level_node = log_root_node->get_children().find("level");
    if (level_node != log_root_node->get_children().end() && level_node->second) {
      dst.set_level(level_node->second->as_cpp_string());

      if (nullptr != dump_existed_set) {
        dump_existed_set->insert("level");
      }
    }
  }

  setup_load_category(*log_root_node, *dst.mutable_category(), dump_existed_set, "");
}

namespace {

static void setup_load_sink(const YAML::Node &log_sink_yaml_src, atapp::protocol::atapp_log_sink &out,
                            configure_key_set *dump_existed_set, gsl::string_view exist_set_prefix) {
  // yaml_loader_dump_to(src, out); // already dumped before in setup_load_category(...)

  if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_file_sink_name().data())) {
    // Inner file sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_file(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stdout_sink_name().data())) {
    // Inner stdout sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_stdout(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_stderr_sink_name().data())) {
    // Inner stderr sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_stderr(), dump_existed_set, exist_set_prefix);
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP(out.type().c_str(), log_sink_maker::get_syslog_sink_name().data())) {
    // Inner syslog sink
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_log_backend_syslog(), dump_existed_set, exist_set_prefix);
  } else {
    // Dump all configures into unresolved_key_values
    yaml_loader_dump_to(log_sink_yaml_src, *out.mutable_unresolved_key_values(), "", dump_existed_set,
                        exist_set_prefix);
  }
}

static void setup_load_category(
    const YAML::Node &src,
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedPtrField<atapp::protocol::atapp_log_category> &out,
    configure_key_set *dump_existed_set, gsl::string_view exist_set_prefix) {
  if (!src || !src.IsMap()) {
    return;
  }

  int32_t index = -1;
  int32_t next_index = 0;
  const YAML::Node name_node = src["name"];
  if (!name_node || !name_node.IsScalar()) {
    return;
  }
  const YAML::Node index_node = src["index"];
  if (index_node && index_node.IsScalar()) {
    if (!index_node.Scalar().empty()) {
      util::string::str2int(index, index_node.Scalar());
    }
  }

  if (name_node.Scalar().empty()) {
    return;
  }

  atapp::protocol::atapp_log_category *log_cat = nullptr;
  for (int i = 0; i < out.size() && nullptr == log_cat; ++i) {
    if (out.Get(i).name() == name_node.Scalar() || index == out.Get(i).index()) {
      log_cat = out.Mutable(i);
    }

    if (out.Get(i).index() >= next_index) {
      next_index = out.Get(i).index() + 1;
    }
  }

  if (nullptr == log_cat) {
    log_cat = out.Add();
    if (nullptr != log_cat) {
      if (index < 0) {
        index = next_index;
      }
      log_cat->set_index(index);
      log_cat->set_name(name_node.Scalar());
    }
  }

  if (nullptr == log_cat) {
    FWLOGERROR("log {} malloc category failed, skipped.", name_node.Scalar());
    return;
  }

  index = log_cat->index();
  std::string exist_set_category_prefix = util::log::format("{}category.{}.", exist_set_prefix, index);
  if (nullptr != dump_existed_set) {
    dump_existed_set->insert(
        static_cast<std::string>(exist_set_category_prefix.substr(0, exist_set_category_prefix.size() - 1)));
  }

  int old_sink_count = log_cat->sink_size();
  yaml_loader_dump_to(src, *log_cat, dump_existed_set, exist_set_category_prefix);

  // Restore index
  log_cat->set_index(index);

  if (nullptr != dump_existed_set) {
    dump_existed_set->insert(util::log::format("{}index", exist_set_category_prefix));
  }

  const YAML::Node sink_node = src["sink"];
  if (!sink_node) {
    return;
  }

  if (sink_node.IsMap() && log_cat->sink_size() > old_sink_count) {
    std::string exist_set_sink_prefix = util::log::format("{}sink.", exist_set_category_prefix);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(
          static_cast<std::string>(exist_set_sink_prefix.substr(0, exist_set_sink_prefix.size() - 1)));
    }
    setup_load_sink(sink_node, *log_cat->mutable_sink(old_sink_count), dump_existed_set, exist_set_sink_prefix);
  } else if (sink_node.IsSequence()) {
    for (int i = 0; i + old_sink_count < log_cat->sink_size(); ++i) {
      if (static_cast<size_t>(i) >= sink_node.size()) {
        break;
      }

      std::string exist_set_sink_prefix = util::log::format("{}sink.{}.", exist_set_category_prefix, i);
      if (nullptr != dump_existed_set) {
        dump_existed_set->insert(
            static_cast<std::string>(exist_set_sink_prefix.substr(0, exist_set_sink_prefix.size() - 1)));
      }

      setup_load_sink(sink_node[i], *log_cat->mutable_sink(i + old_sink_count), dump_existed_set,
                      exist_set_sink_prefix);
    }
  }
}
}  // namespace

void app::parse_yaml_log_categories_into(atapp::protocol::atapp_log &dst, const std::vector<gsl::string_view> &path,
                                         configure_key_set *dump_existed_set) const noexcept {
  for (yaml_conf_map_t::const_iterator iter = yaml_loader_.begin(); iter != yaml_loader_.end(); ++iter) {
    for (size_t i = 0; i < iter->second.size(); ++i) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      try {
#endif
        const YAML::Node log_node = yaml_loader_get_child_by_path(iter->second[i], path);
        if (!log_node || !log_node.IsMap()) {
          continue;
        }
        const YAML::Node log_level_node = log_node["level"];
        if (log_level_node && log_level_node.IsScalar() && !log_level_node.Scalar().empty()) {
          dst.set_level(log_level_node.Scalar());
        }

        if (nullptr != dump_existed_set) {
          dump_existed_set->insert("level");
        }

        const YAML::Node log_category_node = log_node["category"];
        if (!log_category_node) {
          continue;
        }
        if (log_category_node.IsMap()) {
          setup_load_category(log_category_node, *dst.mutable_category(), dump_existed_set, "");
        } else if (log_category_node.IsSequence()) {
          size_t sz = log_category_node.size();
          for (size_t j = 0; j < sz; ++j) {
            const YAML::Node cat_node = log_category_node[j];
            if (!cat_node || !cat_node.IsMap()) {
              continue;
            }
            setup_load_category(cat_node, *dst.mutable_category(), dump_existed_set, "");
          }
        }
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
      } catch (...) {
        // Ignore error
      }
#endif
    }
  }
}

LIBATAPP_MACRO_API int app::trigger_event_on_forward_request(const message_sender_t &source, const message_t &msg) {
  if (evt_on_forward_request_) {
    return evt_on_forward_request_(std::ref(*this), source, msg);
  }

  return 0;
}

LIBATAPP_MACRO_API int app::trigger_event_on_forward_response(const message_sender_t &source, const message_t &msg,
                                                              int32_t error_code) {
  if (evt_on_forward_response_) {
    return evt_on_forward_response_(std::ref(*this), source, msg, error_code);
  }

  return 0;
}

LIBATAPP_MACRO_API void app::trigger_event_on_discovery_event(etcd_discovery_action_t::type action,
                                                              const etcd_discovery_node::ptr_t &node) {
  if (node) {
    const atapp::protocol::atapp_discovery &discovery_info = node->get_discovery_info();
    if (action == etcd_discovery_action_t::EN_NAT_PUT) {
      FWLOGINFO("app {}({}, type={}:{}) got a PUT discovery event({}({}, type={}:{}))", get_app_name(), get_app_id(),
                get_type_id(), get_type_name(), discovery_info.name(), discovery_info.id(), discovery_info.type_id(),
                discovery_info.type_name());
    } else {
      FWLOGINFO("app {}({}, type={}:{}) got a DELETE discovery event({}({}, type={}:{})", get_app_name(), get_app_id(),
                get_type_id(), get_type_name(), discovery_info.name(), discovery_info.id(), discovery_info.type_id(),
                discovery_info.type_name());
    }
  }

  std::lock_guard<std::recursive_mutex> lock(connectors_lock_);
  for (std::list<std::shared_ptr<atapp_connector_impl>>::const_iterator iter = connectors_.begin();
       iter != connectors_.end(); ++iter) {
    if (*iter) {
      (*iter)->on_discovery_event(action, node);
    }
  }
}

int app::setup_signal() {
  // block signals
  app::last_instance_ = this;
  signal(SIGTERM, _app_setup_signal_handle);
  signal(SIGINT, SIG_IGN);

#ifndef WIN32
  signal(SIGSTOP, _app_setup_signal_handle);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGHUP, SIG_IGN);   // lost parent process
  signal(SIGPIPE, SIG_IGN);  // close stdin, stdout or stderr
  signal(SIGTSTP, SIG_IGN);  // close tty
  signal(SIGTTIN, SIG_IGN);  // tty input
  signal(SIGTTOU, SIG_IGN);  // tty output
#endif

  return 0;
}

void app::setup_startup_log() {
  util::log::log_wrapper &wrapper = *WLOG_GETCAT(util::log::log_wrapper::categorize_t::DEFAULT);

  ::atapp::protocol::atapp_log std_log_cfg;
  ::atapp::protocol::atapp_log_category std_cat_cfg;
  ::atapp::protocol::atapp_log_sink std_sink_cfg;

  for (std::list<std::string>::iterator iter = conf_.startup_log.begin(); iter != conf_.startup_log.end(); ++iter) {
    if ((*iter).empty() || 0 == UTIL_STRFUNC_STRNCASE_CMP(log_sink_maker::get_stdout_sink_name().data(),
                                                          (*iter).c_str(), (*iter).size())) {
      wrapper.add_sink(log_sink_maker::get_stdout_sink_reg()(wrapper, util::log::log_wrapper::categorize_t::DEFAULT,
                                                             std_log_cfg, std_cat_cfg, std_sink_cfg));
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(log_sink_maker::get_stderr_sink_name().data(), (*iter).c_str(),
                                              (*iter).size())) {
      wrapper.add_sink(log_sink_maker::get_stderr_sink_reg()(wrapper, util::log::log_wrapper::categorize_t::DEFAULT,
                                                             std_log_cfg, std_cat_cfg, std_sink_cfg));
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP(log_sink_maker::get_syslog_sink_name().data(), (*iter).c_str(),
                                              (*iter).size())) {
      util::log::log_sink_file_backend file_sink(*iter);
      file_sink.set_rotate_size(100 * 1024 * 1024);  // Max 100 MB
      file_sink.set_flush_interval(1);
      wrapper.add_sink(file_sink);
    } else {
      util::log::log_sink_file_backend file_sink(*iter);
      file_sink.set_rotate_size(100 * 1024 * 1024);  // Max 100 MB
      file_sink.set_flush_interval(1);
      wrapper.add_sink(file_sink);
    }
  }

  if (wrapper.sink_size() == 0) {
    wrapper.add_sink(log_sink_maker::get_stdout_sink_reg()(wrapper, util::log::log_wrapper::categorize_t::DEFAULT,
                                                           std_log_cfg, std_cat_cfg, std_sink_cfg));
  }

  wrapper.init();
}

int app::setup_log() {
  util::cli::shell_stream ss(std::cerr);

  // register inner log module
  std::string log_sink_name = static_cast<std::string>(log_sink_maker::get_file_sink_name());
  if (log_reg_.find(log_sink_name) == log_reg_.end()) {
    log_reg_[log_sink_name] = log_sink_maker::get_file_sink_reg();
  }

  log_sink_name = static_cast<std::string>(log_sink_maker::get_stdout_sink_name());
  if (log_reg_.find(log_sink_name) == log_reg_.end()) {
    log_reg_[log_sink_name] = log_sink_maker::get_stdout_sink_reg();
  }

  log_sink_name = static_cast<std::string>(log_sink_maker::get_stderr_sink_name());
  if (log_reg_.find(log_sink_name) == log_reg_.end()) {
    log_reg_[log_sink_name] = log_sink_maker::get_stderr_sink_reg();
  }

  log_sink_name = static_cast<std::string>(log_sink_maker::get_syslog_sink_name());
  if (log_reg_.find(log_sink_name) == log_reg_.end()) {
    log_reg_[log_sink_name] = log_sink_maker::get_syslog_sink_reg();
  }

  if (false == is_running()) {
    // if inited, let all modules setup custom logger
    for (module_ptr_t &mod : modules_) {
      if (mod && mod->is_enabled()) {
        int res = mod->setup_log();
        if (0 != res) {
          FWLOGERROR("Setup log for module {} failed, result: {}", mod->name(), res);
          return res;
        }
      }
    }
  }

  conf_.previous_log.Clear();
  conf_.previous_log.Swap(&conf_.log);
  parse_log_configures_into(conf_.log, std::vector<gsl::string_view>{"atapp", "log"}, "ATAPP_LOG");
  for (int i = 0; i < conf_.log.category_size() && i < util::log::log_wrapper::categorize_t::MAX; ++i) {
    int32_t log_index = conf_.log.category(i).index();
    auto logger = WLOG_GETCAT(log_index);
    if (nullptr == logger) {
      FWLOGERROR("Internal log index {} is invalid, please use {} to create custom logger", log_index,
                 "util::log::log_wrapper::create_user_logger(...)");
      continue;
    }

    setup_logger(*logger, conf_.log.level(), conf_.log.category(i));
  }

  return 0;
}

int app::setup_atbus() {
  int ret = 0, res = 0;
  if (bus_node_) {
    bus_node_->reset();
    bus_node_.reset();
  }

  bus_node_ = atbus::node::create();
  if (!bus_node_) {
    FWLOGERROR("create bus node failed.");
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  ev_loop_t *loop = get_evloop();
  conf_.bus_conf.ev_loop = loop;
  ret = bus_node_->init(conf_.id, &conf_.bus_conf);
  if (ret < 0) {
    FWLOGERROR("init bus node failed. ret: {}", ret);
    bus_node_.reset();
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  // setup all callbacks
  bus_node_->set_on_recv_handle([this](const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                       const ::atbus::protocol::msg &m, const void *data, size_t data_size) -> int {
    return this->bus_evt_callback_on_recv_msg(n, ep, conn, m, data, data_size);
  });

  bus_node_->set_on_forward_response_handle([this](const atbus::node &n, const atbus::endpoint *ep,
                                                   const atbus::connection *conn,
                                                   const ::atbus::protocol::msg *m) -> int {
    return this->bus_evt_callback_on_forward_response(n, ep, conn, m);
  });

  bus_node_->set_on_error_handle([this](const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                        int status, int errorcode) -> int {
    // error log
    return this->bus_evt_callback_on_error(n, ep, conn, status, errorcode);
  });

  bus_node_->set_on_info_log_handle([this](const atbus::node &n, const atbus::endpoint *ep,
                                           const atbus::connection *conn, const char *content) -> int {
    // normal log
    return this->bus_evt_callback_on_info_log(n, ep, conn, content);
  });

  bus_node_->set_on_register_handle(
      [this](const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn, int status) -> int {
        return this->bus_evt_callback_on_reg(n, ep, conn, status);
      });

  bus_node_->set_on_shutdown_handle([this](const atbus::node &n, int reason) -> int {
    // shutdown
    return this->bus_evt_callback_on_shutdown(n, reason);
  });

  bus_node_->set_on_available_handle([this](const atbus::node &n, int result) -> int {
    // available
    return this->bus_evt_callback_on_available(n, result);
  });

  bus_node_->set_on_invalid_connection_handle(
      [this](const atbus::node &n, const atbus::connection *conn, int error_code) -> int {
        return this->bus_evt_callback_on_invalid_connection(n, conn, error_code);
      });

  bus_node_->set_on_custom_cmd_handle([this](const atbus::node &n, const atbus::endpoint *ep,
                                             const atbus::connection *conn, atbus::node::bus_id_t bus_id,
                                             const std::vector<std::pair<const void *, size_t>> &request_body,
                                             std::list<std::string> &response_body) -> int {
    return this->bus_evt_callback_on_custom_command_request(n, ep, conn, bus_id, request_body, response_body);
  });

  bus_node_->set_on_new_connection_handle([this](const atbus::node &n, const atbus::connection *conn) -> int {
    return this->bus_evt_callback_on_new_connection(n, conn);
  });
  bus_node_->set_on_add_endpoint_handle([this](const atbus::node &n, atbus::endpoint *ep, int error_code) -> int {
    return this->bus_evt_callback_on_add_endpoint(n, ep, error_code);
  });

  bus_node_->set_on_remove_endpoint_handle([this](const atbus::node &n, atbus::endpoint *ep, int error_code) -> int {
    return this->bus_evt_callback_on_remove_endpoint(n, ep, error_code);
  });

  // init listen
  for (int i = 0; i < conf_.origin.bus().listen_size(); ++i) {
    res = listen(conf_.origin.bus().listen(i));
    if (res < 0) {
#ifdef _WIN32
      if (EN_ATBUS_ERR_SHM_GET_FAILED == res) {
        FWLOGERROR(
            "Using global shared memory require SeCreateGlobalPrivilege, try to run as Administrator.\nWe will ignore "
            "{} this time.",
            conf_.origin.bus().listen(i));
        util::cli::shell_stream ss(std::cerr);
        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED
             << "Using global shared memory require SeCreateGlobalPrivilege, try to run as Administrator." << std::endl
             << "We will ignore " << conf_.origin.bus().listen(i) << " this time." << std::endl;

        // res = 0; // Value stored to 'res' is never read
      } else {
#endif
        FWLOGERROR("bus node listen {} failed. res: {}", conf_.origin.bus().listen(i), res);
        if (EN_ATBUS_ERR_PIPE_ADDR_TOO_LONG == res) {
          atbus::channel::channel_address_t address;
          atbus::channel::make_address(conf_.origin.bus().listen(i).c_str(), address);
          std::string abs_path = util::file_system::get_abs_path(address.host.c_str());
          FWLOGERROR("listen pipe socket {}, but the length ({}) exceed the limit {}", abs_path, abs_path.size(),
                     atbus::channel::io_stream_get_max_unix_socket_length());
        }
        ret = res;
#ifdef _WIN32
      }
#endif
    }
  }

  if (ret < 0) {
    FWLOGERROR("bus node listen failed");
    bus_node_.reset();
    return ret;
  }

  // start
  ret = bus_node_->start();
  if (ret < 0) {
    FWLOGERROR("bus node start failed, ret: {}", ret);
    bus_node_.reset();
    return ret;
  }

  // if has father node, block and connect to father node
  if (atbus::node::state_t::CONNECTING_PARENT == bus_node_->get_state() ||
      atbus::node::state_t::LOST_PARENT == bus_node_->get_state()) {
    while (nullptr == bus_node_->get_parent_endpoint()) {
      if (check_flag(flag_t::TIMEOUT)) {
        FWLOGERROR("connection to parent node {} timeout", conf_.bus_conf.parent_address);
        ret = -1;
        break;
      }

      flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
      uv_run(loop, UV_RUN_ONCE);
    }

    if (ret < 0) {
      FWLOGERROR("connect to parent node failed");
      bus_node_.reset();
      return ret;
    }
  }

  return 0;
}

void app::close_timer(timer_ptr_t &t) {
  // if timer is maintained by a RAII guard, just ignore.
  if (t) {
    uv_timer_stop(&t->timer);
    t->timer.data = new timer_ptr_t(t);
    uv_close(reinterpret_cast<uv_handle_t *>(&t->timer), _app_close_timer_handle);

    t.reset();
  }
}

static void _app_tick_timer_handle(uv_timer_t *handle) {
  if (nullptr != handle && nullptr != handle->data) {
    std::chrono::system_clock::time_point start_timer = std::chrono::system_clock::now();

    app *self = reinterpret_cast<app *>(handle->data);

    // insert timer again, just like uv_timer_again
    std::chrono::system_clock::duration timer_interval = self->get_configure_timer_interval();

    self->tick();

    std::chrono::system_clock::time_point end_timer = std::chrono::system_clock::now();
    std::chrono::system_clock::duration tick_cost = end_timer - start_timer;
    self->produce_tick_timer_compensation(end_timer - start_timer);

    // It may take a long time to process the tick, so update the time after the tick
    uv_update_time(handle->loop);

    if (tick_cost > timer_interval) {
      uv_timer_start(handle, _app_tick_timer_handle, self->consume_tick_timer_compensation(), 0);
    } else {
      uv_timer_start(handle, _app_tick_timer_handle,
                     chrono_to_libuv_duration(timer_interval - tick_cost) + self->consume_tick_timer_compensation(), 0);
    }
  } else {
    uv_timer_start(handle, _app_tick_timer_handle, 256, 0);
  }
}

int app::setup_tick_timer() {
  close_timer(tick_timer_.tick_timer);

  FWLOGINFO("setup tick interval to {}ms.", chrono_to_libuv_duration(conf_.timer_tick_interval));

  timer_ptr_t timer = std::make_shared<timer_info_t>();
  ev_loop_t *loop = get_evloop();
  assert(loop);
  uv_timer_init(loop, &timer->timer);
  timer->timer.data = this;

  int res =
      uv_timer_start(&timer->timer, _app_tick_timer_handle, chrono_to_libuv_duration(conf_.timer_tick_interval), 0);
  if (0 == res) {
    tick_timer_.tick_timer = timer;
  } else {
    FWLOGERROR("setup tick timer failed, res: {}", res);

    timer->timer.data = new timer_ptr_t(timer);
    uv_close(reinterpret_cast<uv_handle_t *>(&timer->timer), _app_close_timer_handle);

    return EN_ATAPP_ERR_SETUP_TIMER;
  }

  return 0;
}

bool app::setup_timeout_timer(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &duration) {
  if (tick_timer_.timeout_timer) {
    return false;
  }

  if (nullptr == get_evloop()) {
    return false;
  }

  timer_ptr_t timer = std::make_shared<timer_info_t>();
  uv_timer_init(get_evloop(), &timer->timer);
  timer->timer.data = this;

  int res =
      uv_timer_start(&timer->timer, ev_stop_timeout, chrono_to_libuv_duration(duration, ATAPP_DEFAULT_STOP_TIMEOUT), 0);
  if UTIL_LIKELY_CONDITION (0 == res) {
    tick_timer_.timeout_timer = timer;

    return true;
  } else {
    FWLOGERROR("setup timeout timer failed, res: {}", res);
    set_flag(flag_t::TIMEOUT, false);

    timer->timer.data = new timer_ptr_t(timer);
    uv_close(reinterpret_cast<uv_handle_t *>(&timer->timer), _app_close_timer_handle);

    return false;
  }
}

bool app::write_pidfile(int pid) {
  if (!conf_.pid_file.empty()) {
    std::fstream pid_file;
    pid_file.open(conf_.pid_file.c_str(), std::ios::out | std::ios::trunc);
    if (!pid_file.is_open()) {
      util::cli::shell_stream ss(std::cerr);
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "open and write pid file " << conf_.pid_file
           << " failed" << std::endl;
      FWLOGERROR("open and write pid file {} failed", conf_.pid_file);
      // Failed and skip running
      return false;
    } else {
      // Write 0 when start failed
      if (pid < 0) {
        pid_file << 0;
      } else {
        pid_file << pid;
      }
      pid_file.close();
    }
  }

  return true;
}

bool app::cleanup_pidfile() {
  if (conf_.origin.remove_pidfile_after_exit() && !conf_.pid_file.empty()) {
    std::fstream pid_file;

    pid_file.open(conf_.pid_file.c_str(), std::ios::in);
    if (!pid_file.is_open()) {
      util::cli::shell_stream ss(std::cerr);
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "try to remove pid file " << conf_.pid_file
           << " failed" << std::endl;

      // failed and skip running
      return false;
    } else {
      int pid = 0;
      pid_file >> pid;
      pid_file.close();

      if (pid != atbus::node::get_pid()) {
        util::cli::shell_stream ss(std::cerr);
        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << "skip remove pid file " << conf_.pid_file
             << ". because it has pid " << pid << ", but our pid is " << atbus::node::get_pid() << std::endl;

        return false;
      } else {
        return util::file_system::remove(conf_.pid_file.c_str());
      }
    }
  }

  return true;
}

bool app::write_startup_error_file(int error_code) {
  const char *startup_error_file_path =
      conf_.start_error_file.empty() ? conf_.pid_file.c_str() : conf_.start_error_file.c_str();
  if (nullptr != startup_error_file_path && *startup_error_file_path) {
    std::fstream startup_error_file;
    startup_error_file.open(startup_error_file_path, std::ios::out | std::ios::trunc);
    if (!startup_error_file.is_open()) {
      util::cli::shell_stream ss(std::cerr);
      ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "open and write startup error file "
           << startup_error_file_path << " failed" << std::endl;
      FWLOGERROR("open and write startup error file{} failed", startup_error_file_path);
      // Failed and skip running
      return false;
    } else {
      startup_error_file << error_code;
      startup_error_file.close();
    }
  }

  return true;
}

bool app::cleanup_startup_error_file() {
  if (conf_.start_error_file.empty()) {
    std::string start_error_file;
    auto last_dot = conf_.pid_file.find_last_of('.');
    if (last_dot != std::string::npos) {
      start_error_file = conf_.pid_file.substr(0, last_dot) + ".startup-error";
    } else {
      start_error_file = conf_.pid_file + ".startup-error";
    }
    if (!util::file_system::is_exist(start_error_file.c_str())) {
      return true;
    }

    return util::file_system::remove(start_error_file.c_str());
  } else {
    if (!util::file_system::is_exist(conf_.start_error_file.c_str())) {
      return true;
    }

    return util::file_system::remove(conf_.start_error_file.c_str());
  }
}

void app::print_help() {
  util::cli::shell_stream shls(std::cout);

  shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD
         << "Usage: " << conf_.execute_path << " <options> <command> [command paraters...]" << std::endl;
  shls() << get_option_manager()->get_help_msg() << std::endl << std::endl;

  if (!(get_command_manager()->empty() && get_command_manager()->children_empty())) {
    shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD
           << "Custom command help:" << std::endl;
    shls() << get_command_manager()->get_help_msg() << std::endl;
  }
}

bool app::match_gateway_hosts(const atapp::protocol::atapp_gateway &checked) const noexcept {
  bool has_matched_value = false;
  bool has_valid_conf = false;
  for (int i = 0; !has_matched_value && i < checked.match_hosts_size(); ++i) {
    if (checked.match_hosts(i).empty()) {
      continue;
    }
    has_valid_conf = true;
    has_matched_value = checked.match_hosts(i) == atbus::node::get_hostname();
  }

  return !has_valid_conf || has_matched_value;
}

bool app::match_gateway_namespace(const atapp::protocol::atapp_gateway &checked) const noexcept {
  bool has_matched_value = false;
  bool has_valid_conf = false;
  for (int i = 0; !has_matched_value && i < checked.match_namespaces_size(); ++i) {
    if (checked.match_namespaces(i).empty()) {
      continue;
    }
    has_valid_conf = true;
    has_matched_value = checked.match_namespaces(i) == get_metadata().namespace_name();
  }

  return !has_valid_conf || has_matched_value;
}

bool app::match_gateway_labels(const atapp::protocol::atapp_gateway &checked) const noexcept {
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string>::const_iterator iter =
      checked.match_labels().begin();
  for (; iter != checked.match_labels().end(); ++iter) {
    if (iter->first.empty() || iter->second.empty()) {
      continue;
    }

    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string>::const_iterator self_label_iter =
        get_metadata().labels().find(iter->first);
    if (self_label_iter == get_metadata().labels().end()) {
      return false;
    }
    if (self_label_iter->second != iter->second) {
      return false;
    }
  }

  return true;
}

LIBATAPP_MACRO_API app::custom_command_sender_t app::get_custom_command_sender(util::cli::callback_param params) {
  custom_command_sender_t ret;
  ret.self = nullptr;
  ret.response = nullptr;
  if (nullptr != params.get_ext_param()) {
    ret = *reinterpret_cast<custom_command_sender_t *>(params.get_ext_param());
  }

  return ret;
}

LIBATAPP_MACRO_API bool app::add_custom_command_rsp(util::cli::callback_param params, const std::string &rsp_text) {
  custom_command_sender_t sender = get_custom_command_sender(params);
  if (nullptr == sender.response) {
    return false;
  }

  sender.response->push_back(rsp_text);
  return true;
}

LIBATAPP_MACRO_API void app::split_ids_by_string(const char *in, std::vector<app_id_t> &out) {
  if (nullptr == in) {
    return;
  }

  out.reserve(8);

  while (nullptr != in && *in) {
    // skip spaces
    if (' ' == *in || '\t' == *in || '\r' == *in || '\n' == *in) {
      ++in;
      continue;
    }

    out.push_back(::util::string::to_int<app_id_t>(in));

    for (; nullptr != in && *in && '.' != *in; ++in) {
      continue;
    }
    // skip dot and ready to next segment
    if (nullptr != in && *in && '.' == *in) {
      ++in;
    }
  }
}

LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in,
                                                               const std::vector<app_id_t> &mask_in) {
  if (nullptr == id_in || 0 == *id_in) {
    return 0;
  }

  bool id_in_is_number = true;
  if (!mask_in.empty()) {
    for (const char *check_char = id_in; *check_char && id_in_is_number; ++check_char) {
      if ('.' == *check_char) {
        id_in_is_number = false;
      }
    }
  }

  if (id_in_is_number) {
    return ::util::string::to_int<app_id_t>(id_in);
  }

  std::vector<app_id_t> ids;
  split_ids_by_string(id_in, ids);
  app_id_t ret = 0;
  for (size_t i = 0; i < ids.size() && i < mask_in.size(); ++i) {
    ret <<= mask_in[i];
    ret |= (ids[i] & ((static_cast<app_id_t>(1) << mask_in[i]) - 1));
  }

  return ret;
}

LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in, const char *mask_in) {
  if (nullptr == id_in || 0 == *id_in) {
    return 0;
  }

  std::vector<app_id_t> mask;
  split_ids_by_string(mask_in, mask);
  return convert_app_id_by_string(id_in, mask);
}

LIBATAPP_MACRO_API std::string app::convert_app_id_to_string(app_id_t id_in, const std::vector<app_id_t> &mask_in,
                                                             bool hex) {
  std::vector<app_id_t> ids;
  ids.resize(mask_in.size(), 0);

  for (size_t i = 0; i < mask_in.size(); ++i) {
    size_t idx = mask_in.size() - i - 1;
    ids[idx] = id_in & ((static_cast<app_id_t>(1) << mask_in[idx]) - 1);
    id_in >>= mask_in[idx];
  }

  std::stringstream ss;
  if (hex) {
    ss << std::hex;
  }

  for (size_t i = 0; i < ids.size(); ++i) {
    if (0 != i) {
      ss << ".";
    }

    if (hex) {
      ss << "0x";
    }
    ss << ids[i];
  }

  return ss.str();
}

LIBATAPP_MACRO_API std::string app::convert_app_id_to_string(app_id_t id_in, const char *mask_in, bool hex) {
  std::vector<app_id_t> mask;
  split_ids_by_string(mask_in, mask);
  return convert_app_id_to_string(id_in, mask, hex);
}

LIBATAPP_MACRO_API app *app::get_last_instance() { return last_instance_; }

int app::prog_option_handler_help(util::cli::callback_param /*params*/, util::cli::cmd_option * /*opt_mgr*/,
                                  util::cli::cmd_option_ci * /*cmd_mgr*/) {
  mode_ = mode_t::HELP;
  return 0;
}

int app::prog_option_handler_version(util::cli::callback_param /*params*/) {
  mode_ = mode_t::INFO;
  printf("%s", get_build_version().c_str());
  return 0;
}

int app::prog_option_handler_set_id(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.id_cmd = params[0]->to_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-id require 1 parameter" << std::endl;
  }

  return 0;
}

int app::prog_option_handler_set_id_mask(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.id_mask.clear();
    split_ids_by_string(params[0]->to_string(), conf_.id_mask);
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-id-mask require 1 parameter" << std::endl;
  }

  return 0;
}

int app::prog_option_handler_set_conf_file(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.conf_file = params[0]->to_cpp_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-c, --conf, --config require 1 parameter"
         << std::endl;
  }

  return 0;
}

int app::prog_option_handler_set_pid(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.pid_file = params[0]->to_cpp_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-p, --pid require 1 parameter" << std::endl;
  }

  return 0;
}

int app::prog_option_handler_upgrade_mode(util::cli::callback_param /*params*/) {
  conf_.upgrade_mode = true;
  return 0;
}

int app::prog_option_handler_set_startup_log(util::cli::callback_param params) {
  for (util::cli::cmd_option_list::size_type i = 0; i < params.get_params_number(); ++i) {
    conf_.startup_log.push_back(params[i]->to_cpp_string());
  }
  return 0;
}

int app::prog_option_handler_set_startup_error_file(util::cli::callback_param params) {
  if (params.get_params_number() > 0) {
    conf_.start_error_file = params[0]->to_cpp_string();
  } else {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "--startup-error-file require 1 parameter"
         << std::endl;
  }

  return 0;
}

int app::prog_option_handler_start(util::cli::callback_param /*params*/) {
  mode_ = mode_t::START;
  return 0;
}

int app::prog_option_handler_stop(util::cli::callback_param /*params*/) {
  mode_ = mode_t::STOP;
  last_command_.clear();
  last_command_.push_back("stop");
  if (conf_.upgrade_mode) {
    last_command_.push_back("--upgrade");
  }
  return 0;
}

int app::prog_option_handler_reload(util::cli::callback_param /*params*/) {
  mode_ = mode_t::RELOAD;
  last_command_.clear();
  last_command_.push_back("reload");
  if (conf_.upgrade_mode) {
    last_command_.push_back("--upgrade");
  }
  return 0;
}

int app::prog_option_handler_run(util::cli::callback_param params) {
  mode_ = mode_t::CUSTOM;
  for (size_t i = 0; i < params.get_params_number(); ++i) {
    last_command_.push_back(params[i]->to_cpp_string());
  }

  if (0 == params.get_params_number()) {
    mode_ = mode_t::INFO;
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "run must follow a command" << std::endl;
  }
  return 0;
}

void app::setup_option(int argc, const char *argv[], void *priv_data) {
  assert(argc > 0);

  util::cli::cmd_option::ptr_type opt_mgr = get_option_manager();
  util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();
  // show help and exit
  opt_mgr->bind_cmd("-h, --help, help", &app::prog_option_handler_help, this, opt_mgr.get(), cmd_mgr.get())
      ->set_help_msg("-h. --help, help                       show this help message.");

  // show version and exit
  opt_mgr->bind_cmd("-v, --version", &app::prog_option_handler_version, this)
      ->set_help_msg("-v, --version                          show version and exit.");

  // set app bus id
  opt_mgr->bind_cmd("-id", &app::prog_option_handler_set_id, this)
      ->set_help_msg("-id <bus id>                           set app bus id.");
  // set app bus id
  opt_mgr->bind_cmd("-id-mask", &app::prog_option_handler_set_id_mask, this)
      ->set_help_msg(
          "-id-mask <bit number of bus id mask>   set app bus id mask(example: 8.8.8.8, and then -id 1.2.3.4 is just "
          "like "
          "-id 0x01020304).");

  // set configure file path
  opt_mgr->bind_cmd("-c, --conf, --config", &app::prog_option_handler_set_conf_file, this)
      ->set_help_msg("-c, --conf, --config <file path>       set configure file path.");

  // set app pid file
  opt_mgr->bind_cmd("-p, --pid", &app::prog_option_handler_set_pid, this)
      ->set_help_msg("-p, --pid <pid file>                   set where to store pid.");

  // set configure file path
  opt_mgr->bind_cmd("--upgrade", &app::prog_option_handler_upgrade_mode, this)
      ->set_help_msg("--upgrade                              set upgrade mode.");

  opt_mgr->bind_cmd("--startup-log", &app::prog_option_handler_set_startup_log, this)
      ->set_help_msg("--startup-log                          where to write start up log(file name or stdout/stderr).");

  // set startup error file
  opt_mgr->bind_cmd("--startup-error-file", &app::prog_option_handler_set_startup_error_file, this)
      ->set_help_msg("--startup-error-file <file path>       set where to store startup error code.");

  // start server
  opt_mgr->bind_cmd("start", &app::prog_option_handler_start, this)
      ->set_help_msg("start                                  start mode.");

  // stop server
  opt_mgr->bind_cmd("stop", &app::prog_option_handler_stop, this)
      ->set_help_msg("stop                                   send stop command to server.");

  // reload all configures
  opt_mgr->bind_cmd("reload", &app::prog_option_handler_reload, this)
      ->set_help_msg("reload                                 send reload command to server.");

  // run custom command
  opt_mgr->bind_cmd("run", &app::prog_option_handler_run, this)
      ->set_help_msg("run <command> [parameters...]          send custom command and parameters to server.");

  conf_.execute_path = argv[0];
  // fill app version data
  if (conf_.app_version.empty()) {
    std::vector<std::string> out;
    util::file_system::split_path(out, conf_.execute_path);
    std::stringstream ss;
    if (out.empty()) {
      ss << conf_.execute_path;
    } else {
      ss << out[out.size() - 1];
    }
    ss << " with libatapp " << LIBATAPP_VERSION;
    conf_.app_version = ss.str();
  }
  opt_mgr->start(argc - 1, &argv[1], false, priv_data);
}

int app::app::command_handler_start(util::cli::callback_param /*params*/) {
  // add_custom_command_rsp(params, "success");
  // do nothing
  return 0;
}

int app::command_handler_stop(util::cli::callback_param params) {
  char msg[256] = {0};
  LOG_WRAPPER_FWAPI_FORMAT_TO_N(msg, sizeof(msg), "app node {:#x} run stop command success", get_app_id());
  FWLOGINFO("{}", msg);
  add_custom_command_rsp(params, msg);

  bool enable_upgrade_mode = false;
  for (util::cli::cmd_option_list::size_type i = 0; i < params.get_params_number(); ++i) {
    if (params[i]->to_cpp_string() == "--upgrade") {
      enable_upgrade_mode = true;
    }
  }
  conf_.upgrade_mode = enable_upgrade_mode;
  return stop();
}

int app::command_handler_reload(util::cli::callback_param params) {
  bool enable_upgrade_mode = false;
  for (util::cli::cmd_option_list::size_type i = 0; i < params.get_params_number(); ++i) {
    if (params[i]->to_cpp_string() == "--upgrade") {
      enable_upgrade_mode = true;
    }
  }
  conf_.upgrade_mode = enable_upgrade_mode;

  std::chrono::system_clock::time_point previous_timepoint = std::chrono::system_clock::now();
  int ret = reload();
  std::chrono::system_clock::time_point current_timepoint = std::chrono::system_clock::now();

  for (auto &reload_module : stats_.module_reload) {
    if (ret >= 0 && reload_module.result < 0) {
      ret = reload_module.result;
    }
  }

  char msg[1024];
  if (ret >= 0) {
    for (auto &reload_module : stats_.module_reload) {
      const char *module_name = reload_module.module ? reload_module.module->name() : "[UNKNOWN]";
      auto format_result = LOG_WRAPPER_FWAPI_FORMAT_TO_N(
          msg, sizeof(msg), "app {}({:#x}) module {}({}) reload cost {}us, result: {}", get_app_name(), get_app_id(),
          module_name, reinterpret_cast<const void *>(reload_module.module.get()),
          std::chrono::duration_cast<std::chrono::microseconds>(reload_module.cost).count(), reload_module.result);
      if (format_result.size > 0 && format_result.size < sizeof(msg)) {
        msg[format_result.size] = 0;
      } else {
        msg[sizeof(msg) - 1] = 0;
      }
      FWLOGINFO("{}", msg);
      add_custom_command_rsp(params, msg);
    }
    {
      auto format_result = LOG_WRAPPER_FWAPI_FORMAT_TO_N(
          msg, sizeof(msg), "app {}({:#x}) run reload command success.({}us)", get_app_name(), get_app_id(),
          std::chrono::duration_cast<std::chrono::microseconds>(current_timepoint - previous_timepoint).count());
      if (format_result.size > 0 && format_result.size < sizeof(msg)) {
        msg[format_result.size] = 0;
      } else {
        msg[sizeof(msg) - 1] = 0;
      }
    }
    FWLOGINFO("{}", msg);
  } else {
    for (auto &reload_module : stats_.module_reload) {
      const char *module_name = reload_module.module ? reload_module.module->name() : "[UNKNOWN]";
      auto format_result = LOG_WRAPPER_FWAPI_FORMAT_TO_N(
          msg, sizeof(msg), "app {}({:#x}) module {}({}) reload cost {}us, result: {}", get_app_name(), get_app_id(),
          module_name, reinterpret_cast<const void *>(reload_module.module.get()),
          std::chrono::duration_cast<std::chrono::microseconds>(reload_module.cost).count(), reload_module.result);
      if (format_result.size > 0 && format_result.size < sizeof(msg)) {
        msg[format_result.size] = 0;
      } else {
        msg[sizeof(msg) - 1] = 0;
      }
      FWLOGWARNING("{}", msg);
      add_custom_command_rsp(params, msg);
    }

    {
      auto format_result = LOG_WRAPPER_FWAPI_FORMAT_TO_N(
          msg, sizeof(msg), "app {}({:#x}) run reload command failed.({}us)", get_app_name(), get_app_id(),
          std::chrono::duration_cast<std::chrono::microseconds>(current_timepoint - previous_timepoint).count());
      if (format_result.size > 0 && format_result.size < sizeof(msg)) {
        msg[format_result.size] = 0;
      } else {
        msg[sizeof(msg) - 1] = 0;
      }
    }
    FWLOGERROR("{}", msg);
  }
  add_custom_command_rsp(params, msg);
  return ret;
}

int app::command_handler_invalid(util::cli::callback_param params) {
  char msg[256] = {0};
  std::stringstream args;
  for (util::cli::cmd_option_list::cmd_array_type::const_iterator iter = params.get_cmd_array().begin();
       iter != params.get_cmd_array().end(); ++iter) {
    args << " \"" << iter->first << '"';
  }
  for (size_t i = 0; i < params.get_params_number(); ++i) {
    if (params[i]) {
      args << " \"" << params[i]->to_cpp_string() << '"';
    }
  }
  LOG_WRAPPER_FWAPI_FORMAT_TO_N(msg, sizeof(msg), "receive invalid command :{}", args.str());
  FWLOGERROR("{}", msg);
  add_custom_command_rsp(params, msg);
  return 0;
}

int app::command_handler_disable_etcd(util::cli::callback_param params) {
  if (!internal_module_etcd_) {
    const char *msg = "Etcd module is not initialized, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  } else if (internal_module_etcd_->is_etcd_enabled()) {
    internal_module_etcd_->disable_etcd();
    const char *msg = "Etcd context is disabled now.";
    FWLOGINFO("{}", msg);
    add_custom_command_rsp(params, msg);
  } else {
    const char *msg = "Etcd context is already disabled, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  }
  return 0;
}

int app::command_handler_enable_etcd(util::cli::callback_param params) {
  if (!internal_module_etcd_) {
    const char *msg = "Etcd module not initialized, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  } else if (internal_module_etcd_->is_etcd_enabled()) {
    const char *msg = "Etcd context is already enabled, skip command.";
    FWLOGERROR("{}", msg);
    add_custom_command_rsp(params, msg);
  } else {
    internal_module_etcd_->enable_etcd();
    if (internal_module_etcd_->is_etcd_enabled()) {
      const char *msg = "Etcd context is enabled now.";
      FWLOGINFO("{}", msg);
      add_custom_command_rsp(params, msg);
    } else {
      const char *msg = "Etcd context can not be enabled, maybe need configure etcd.hosts.";
      FWLOGERROR("{}", msg);
      add_custom_command_rsp(params, msg);
    }
  }

  return 0;
}

int app::command_handler_list_discovery(util::cli::callback_param params) {
  if (!internal_module_etcd_) {
    const char *msg = "Etcd module not initialized.";
    add_custom_command_rsp(params, msg);
  } else {
    size_t start_idx = 0;
    size_t end_idx = 0;
    if (params.get_params_number() > 0) {
      start_idx = static_cast<size_t>(params[0]->to_uint64());
    }
    if (params.get_params_number() > 1) {
      end_idx = static_cast<size_t>(params[1]->to_uint64());
    }

    const std::vector<etcd_discovery_node::ptr_t> &nodes =
        internal_module_etcd_->get_global_discovery().get_sorted_nodes();
    for (size_t i = start_idx; i < nodes.size() && (0 == end_idx || i < end_idx); ++i) {
      if (!nodes[i]) {
        continue;
      }
      const atapp::protocol::atapp_discovery &node_info = nodes[i]->get_discovery_info();
      add_custom_command_rsp(
          params, LOG_WRAPPER_FWAPI_FORMAT("node -> private data: {}, destroy event: {}, hash: {:016x}{:016x}, {}",
                                           reinterpret_cast<const void *>(nodes[i]->get_private_data_ptr()),
                                           (nodes[i]->get_on_destroy() ? "ON" : "OFF"), nodes[i]->get_name_hash().first,
                                           nodes[i]->get_name_hash().second, rapidjson_loader_stringify(node_info)));
    }
  }

  return 0;
}

int app::bus_evt_callback_on_recv_msg(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                      const atbus::protocol::msg &msg, const void *buffer, size_t len) {
  if (atbus::protocol::msg::kDataTransformReq != msg.msg_body_case() || 0 == msg.head().src_bus_id()) {
    FWLOGERROR("receive a message from unknown source {} or invalid body case", msg.head().src_bus_id());
    return EN_ATBUS_ERR_BAD_DATA;
  }

  app_id_t from_id = msg.data_transform_req().from();
  app::message_t message;
  message.data = buffer;
  message.data_size = len;
  message.metadata = nullptr;
  message.message_sequence = msg.head().sequence();
  message.type = msg.head().type();

  app::message_sender_t sender;
  sender.id = from_id;
  sender.remote = get_endpoint(from_id);
  if (nullptr != sender.remote) {
    sender.name = sender.remote->get_name();
  }

  int res = trigger_event_on_forward_request(sender, message);
  if (res < 0) {
    FWLOGERROR("{} forward data {}(type={}, sequence={}) bytes failed, error code: {}",
               atbus_connector_ ? atbus_connector_->name() : "[null]", len, message.type, message.message_sequence,
               res);
  } else {
    FWLOGDEBUG("{} forward data {}(type={}, sequence={}) bytes success, result code: {}",
               atbus_connector_ ? atbus_connector_->name() : "[null]", len, message.type, message.message_sequence,
               res);
  }

  ++stats_.last_proc_event_count;
  return 0;
}

int app::bus_evt_callback_on_forward_response(const atbus::node &, const atbus::endpoint *, const atbus::connection *,
                                              const atbus::protocol::msg *m) {
  ++stats_.last_proc_event_count;

  // call failed callback if it's message transfer
  if (nullptr == m) {
    FWLOGERROR("app {:#x} receive a send failure without message", get_app_id());
    return EN_ATAPP_ERR_SEND_FAILED;
  }

  if (m->head().ret() < 0) {
    FWLOGERROR("app {:#x} receive a send failure from {:#x}, message cmd: {}, type: {}, ret: {}, sequence: {}",
               get_app_id(), m->head().src_bus_id(), atbus::msg_handler::get_body_name(m->msg_body_case()),
               m->head().type(), m->head().ret(), m->head().sequence());
  }

  if (atbus::protocol::msg::kDataTransformRsp != m->msg_body_case() || 0 == m->head().src_bus_id()) {
    FWLOGERROR("receive a message from unknown source {} or invalid body case", m->head().src_bus_id());
    return EN_ATBUS_ERR_BAD_DATA;
  }

  if (atbus_connector_) {
    atbus_connector_->on_receive_forward_response(
        m->data_transform_rsp().from(), m->head().type(), m->head().sequence(), m->head().ret(),
        reinterpret_cast<const void *>(m->data_transform_rsp().content().c_str()),
        m->data_transform_rsp().content().size(), nullptr);
    return 0;
  }

  app_id_t from_id = m->data_transform_rsp().from();
  app::message_t message;
  message.data = reinterpret_cast<const void *>(m->data_transform_rsp().content().c_str());
  message.data_size = m->data_transform_rsp().content().size();
  message.metadata = nullptr;
  message.message_sequence = m->head().sequence();
  message.type = m->head().type();

  app::message_sender_t sender;
  sender.id = from_id;
  sender.remote = get_endpoint(from_id);
  if (nullptr != sender.remote) {
    sender.name = sender.remote->get_name();
  }

  trigger_event_on_forward_response(sender, message, m->head().ret());
  return 0;
}

int app::bus_evt_callback_on_error(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                   int status, int errcode) {
  // meet eof or reset by peer is not a error
  if (UV_EOF == errcode || UV_ECONNRESET == errcode) {
    const char *msg = UV_EOF == errcode ? "got EOF" : "reset by peer";
    if (nullptr != conn) {
      if (nullptr != ep) {
        FWLOGINFO("bus node {:#x} endpoint {:#x} connection {}({}) closed: {}", n.get_id(), ep->get_id(),
                  reinterpret_cast<const void *>(conn), conn->get_address().address, msg);
      } else {
        FWLOGINFO("bus node {:#x} connection {}({}) closed: {}", n.get_id(), reinterpret_cast<const void *>(conn),
                  conn->get_address().address, msg);
      }

    } else {
      if (nullptr != ep) {
        FWLOGINFO("bus node {:#x} endpoint {:#x} closed: {}", n.get_id(), ep->get_id(), msg);
      } else {
        FWLOGINFO("bus node {:#x} closed: {}", n.get_id(), msg);
      }
    }
    return 0;
  }

  if (nullptr != conn) {
    if (nullptr != ep) {
      FWLOGERROR("bus node {:#x} endpoint {:#x} connection {}({}) error, status: {}, error code: {}", n.get_id(),
                 ep->get_id(), reinterpret_cast<const void *>(conn), conn->get_address().address, status, errcode);
    } else {
      FWLOGERROR("bus node {:#x} connection {}({}) error, status: {}, error code: {}", n.get_id(),
                 reinterpret_cast<const void *>(conn), conn->get_address().address, status, errcode);
    }

  } else {
    if (nullptr != ep) {
      FWLOGERROR("bus node {:#x} endpoint {:#x} error, status: {}, error code: {}", n.get_id(), ep->get_id(), status,
                 errcode);
    } else {
      FWLOGERROR("bus node {:#x} error, status: {}, error code: {}", n.get_id(), status, errcode);
    }
  }

  return 0;
}

int app::bus_evt_callback_on_info_log(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                      const char *msg) {
  FWLOGINFO("bus node {:#x} endpoint {:#x}({}) connection {}({}) message: {}", n.get_id(),
            (nullptr == ep ? 0 : ep->get_id()), reinterpret_cast<const void *>(ep),
            reinterpret_cast<const void *>(conn), (nullptr == conn ? "" : conn->get_address().address.c_str()),
            (nullptr == msg ? "" : msg));
  return 0;
}

int app::bus_evt_callback_on_reg(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn,
                                 int res) {
  ++stats_.last_proc_event_count;

  if (nullptr != conn) {
    if (nullptr != ep) {
      FWLOGINFO("bus node {:#x} endpoint {:#x} connection {}({}) registered, res: {}", n.get_id(), ep->get_id(),
                reinterpret_cast<const void *>(conn), conn->get_address().address, res);
    } else {
      FWLOGINFO("bus node {:#x} connection {}({}) registered, res: {}", n.get_id(),
                reinterpret_cast<const void *>(conn), conn->get_address().address, res);
    }

  } else {
    if (nullptr != ep) {
      FWLOGINFO("bus node {:#x} endpoint {:#x} registered, res: {}", n.get_id(), ep->get_id(), res);
    } else {
      FWLOGINFO("bus node {:#x} registered, res: {}", n.get_id(), res);
    }
  }

  if (nullptr != ep && atbus_connector_) {
    atbus_connector_->on_update_endpoint(n, ep, 0);
  }

  return 0;
}

int app::bus_evt_callback_on_shutdown(const atbus::node &n, int reason) {
  FWLOGINFO("bus node {:#x} shutdown, reason: {}", n.get_id(), reason);
  return stop();
}

int app::bus_evt_callback_on_available(const atbus::node &n, int res) {
  FWLOGINFO("bus node {:#x} initialze done, res: {}", n.get_id(), res);
  return res;
}

int app::bus_evt_callback_on_invalid_connection(const atbus::node &n, const atbus::connection *conn, int res) {
  ++stats_.last_proc_event_count;

  if (nullptr == conn) {
    FWLOGERROR("bus node {:#x} recv a invalid nullptr connection , res: {}", n.get_id(), res);
  } else {
    // already disconncted finished.
    if (atbus::connection::state_t::DISCONNECTED != conn->get_status()) {
      if (is_closing()) {
        FWLOGINFO(
            "bus node {:#x} make a invalid connection {}({}) when closing, all unfinished connection will be aborted. "
            "res: {}",
            n.get_id(), reinterpret_cast<const void *>(conn), conn->get_address().address, res);
      } else {
        if (conn->check_flag(atbus::connection::flag_t::TEMPORARY)) {
          FWLOGWARNING("bus node {:#x} temporary connection {}({}) expired. res: {}", n.get_id(),
                       reinterpret_cast<const void *>(conn), conn->get_address().address, res);
        } else {
          FWLOGERROR("bus node {:#x} make a invalid connection {}({}). res: {}", n.get_id(),
                     reinterpret_cast<const void *>(conn), conn->get_address().address, res);
        }
      }
    }
  }
  return 0;
}

int app::bus_evt_callback_on_custom_command_request(const atbus::node &n, const atbus::endpoint *,
                                                    const atbus::connection *, app_id_t /*src_id*/,
                                                    const std::vector<std::pair<const void *, size_t>> &args,
                                                    std::list<std::string> &rsp) {
  ++stats_.last_proc_event_count;
  ++stats_.receive_custom_command_request_count;
  if (args.empty()) {
    return 0;
  }

  std::vector<std::string> args_str;
  std::stringstream ss_log;
  args_str.resize(args.size());

  for (size_t i = 0; i < args_str.size(); ++i) {
    args_str[i].assign(reinterpret_cast<const char *>(args[i].first), args[i].second);
    if (args_str[i].size() > 256) {
      ss_log << " ";
      ss_log.write(args_str[i].c_str(), 64);
      ss_log << "...";
      ss_log.write(args_str[i].c_str() + args_str[i].size() - 64, 64);
    } else {
      ss_log << " " << args_str[i];
    }
  }
  FWLOGINFO("app {}({:#x}) receive a command:{}", get_app_name(), get_app_id(), ss_log.str());

  util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();
  custom_command_sender_t sender;
  sender.self = this;
  sender.response = &rsp;
  cmd_mgr->start(args_str, true, &sender);

  size_t max_size = n.get_conf().msg_size;
  size_t use_size = 0;
  size_t sum_size = 0;
  bool is_truncated = false;
  constexpr const size_t command_reserve_header = 8;  // 8 bytes for libatbus command header.
  for (std::list<std::string>::iterator iter = rsp.begin(); iter != rsp.end();) {
    std::list<std::string>::iterator cur = iter++;
    sum_size += (*cur).size();
    if (is_truncated) {
      rsp.erase(cur);
      continue;
    }
    if (use_size + cur->size() + command_reserve_header >= max_size) {
      if (use_size + command_reserve_header >= max_size) {
        rsp.erase(cur);
      } else {
        cur->resize(max_size - use_size - command_reserve_header);
      }
      use_size = max_size;
      is_truncated = true;
    } else {
      use_size += (*cur).size() + command_reserve_header;
    }
  }

  if (is_truncated) {
    std::stringstream ss;
    ss << "Response message size " << sum_size << " is greater than size limit " << max_size
       << ", some data will be truncated.";
    rsp.push_back(ss.str());
  }
  return 0;
}

int app::bus_evt_callback_on_new_connection(const atbus::node &n, const atbus::connection *conn) {
  if (nullptr == conn) {
    return 0;
  }

  if (conn->is_connected() && conn->get_binding() != nullptr && atbus_connector_) {
    return atbus_connector_->on_update_endpoint(n, conn->get_binding(), 0);
  }

  return 0;
}

int app::bus_evt_callback_on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
  ++stats_.last_proc_event_count;

  if (nullptr == ep) {
    FWLOGERROR("bus node {:#x} make connection to nullptr, res: {}", n.get_id(), res);
  } else {
    FWLOGINFO("bus node {:#x} make connection to {:#x} done, res: {}", n.get_id(), ep->get_id(), res);
    if (atbus_connector_) {
      atbus_connector_->on_add_endpoint(n, ep, res);
    }

    if (evt_on_app_connected_) {
      evt_on_app_connected_(std::ref(*this), std::ref(*ep), res);
    }
  }
  return 0;
}

int app::bus_evt_callback_on_remove_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
  ++stats_.last_proc_event_count;

  if (nullptr == ep) {
    FWLOGERROR("bus node {:#x} release connection to nullptr, res: {}", n.get_id(), res);
  } else {
    FWLOGINFO("bus node {:#x} release connection to {:#x} done, res: {}", n.get_id(), ep->get_id(), res);
    if (atbus_connector_) {
      atbus_connector_->on_remove_endpoint(n, ep, res);
    }

    if (evt_on_app_disconnected_) {
      evt_on_app_disconnected_(std::ref(*this), std::ref(*ep), res);
    }
  }
  return 0;
}

int app::bus_evt_callback_on_custom_command_response(const atbus::node &, const atbus::endpoint *,
                                                     const atbus::connection *, app_id_t src_id,
                                                     const std::vector<std::pair<const void *, size_t>> &args,
                                                     uint64_t /*seq*/) {
  ++stats_.last_proc_event_count;
  ++stats_.receive_custom_command_reponse_count;
  if (args.empty()) {
    return 0;
  }

  util::cli::shell_stream ss(std::cout);
  for (size_t i = 0; i < args.size(); ++i) {
    std::string text(static_cast<const char *>(args[i].first), args[i].second);
    ss() << "Custom Command: (" << LOG_WRAPPER_FWAPI_FORMAT("{:#x}", src_id) << "): " << text << std::endl;
  }

  return 0;
}

void app::app_evt_on_finally() {
  while (!evt_on_finally_.empty()) {
    callback_fn_on_finally_list_t fns;
    fns.swap(evt_on_finally_);
    for (auto &fn : fns) {
      fn(*this);
    }
  }
}

LIBATAPP_MACRO_API void app::add_connector_inner(std::shared_ptr<atapp_connector_impl> connector) {
  if (!connector || check_flag(flag_t::DESTROYING)) {
    return;
  }

  std::lock_guard<std::recursive_mutex> lock_guard{connectors_lock_};

  connectors_.push_back(connector);

  // record all protocols
  for (atapp_connector_impl::protocol_set_t::const_iterator iter = connector->get_support_protocols().begin();
       iter != connector->get_support_protocols().end(); ++iter) {
    connector_protocol_map_t::const_iterator find_iter = connector_protocols_.find(*iter);
    if (find_iter != connector_protocols_.end()) {
      FWLOGWARNING("protocol {} is already registered by {}, we will overwrite it with {}", *iter,
                   find_iter->second->name(), connector->name());
    }

    connector_protocols_[*iter] = connector;
  }
}

void app::setup_command() {
  util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();

  // start server
  cmd_mgr->bind_cmd("start", &app::app::command_handler_start, this);
  // stop server
  cmd_mgr->bind_cmd("stop", &app::app::command_handler_stop, this);
  // reload all configures
  cmd_mgr->bind_cmd("reload", &app::app::command_handler_reload, this);
  // enable etcd
  cmd_mgr->bind_cmd("enable-etcd", &app::command_handler_enable_etcd, this)
      ->set_help_msg("enable-etcd                            enable etcd discovery module.");

  // disable etcd
  cmd_mgr->bind_cmd("disable-etcd", &app::command_handler_disable_etcd, this)
      ->set_help_msg("disable-etcd                           disable etcd discovery module.");

  cmd_mgr->bind_cmd("list-discovery", &app::command_handler_list_discovery, this)
      ->set_help_msg("list-discovery [start:0] [end]         list all discovery node.");

  // invalid command
  cmd_mgr->bind_cmd("@OnError", &app::command_handler_invalid, this);
}

int app::send_last_command(ev_loop_t *ev_loop) {
  if (last_command_.empty()) {
    FWLOGERROR("command is empty.");
    return EN_ATAPP_ERR_COMMAND_IS_NULL;
  }

  // step 1. using the fastest way to connect to server
  int use_level = 0;
  bool is_sync_channel = false;
  atbus::channel::channel_address_t use_addr;

  for (int i = 0; i < conf_.origin.bus().listen_size(); ++i) {
    atbus::channel::channel_address_t parsed_addr;
    make_address(conf_.origin.bus().listen(i).c_str(), parsed_addr);
    int parsed_level = 0;
    if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm", parsed_addr.scheme.c_str(), 3)) {
      parsed_level = 5;
      is_sync_channel = true;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("unix", parsed_addr.scheme.c_str(), 4)) {
      parsed_level = 4;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("ipv6", parsed_addr.scheme.c_str(), 4)) {
      parsed_level = 3;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("ipv4", parsed_addr.scheme.c_str(), 4)) {
      parsed_level = 2;
    } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("dns", parsed_addr.scheme.c_str(), 3)) {
      parsed_level = 1;
    }

    if (parsed_level > use_level) {
#ifdef _WIN32
      // On windows, shared memory must be load in the same directory, so use IOS first
      // Use Global\\ prefix requires the SeCreateGlobalPrivilege privilege
      if (5 == parsed_level && 0 != use_level) {
        continue;
      }
#endif
      use_addr = parsed_addr;
      use_level = parsed_level;
    }
  }

  if (0 == use_level) {
    FWLOGERROR("there is no available listener address to send command.");
    return EN_ATAPP_ERR_NO_AVAILABLE_ADDRESS;
  }

  if (!ev_loop_) {
    ev_loop_ = uv_default_loop();
  }
  conf_.bus_conf.ev_loop = ev_loop_;

  if (!bus_node_) {
    bus_node_ = atbus::node::create();
  }

  // command mode , must no concurrence
  if (!bus_node_) {
    FWLOGERROR("create bus node failed");
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  // no need to connect to parent node
  conf_.bus_conf.parent_address.clear();

  // using 0 for command sender
  int ret = bus_node_->init(0, &conf_.bus_conf);
  if (ret < 0) {
    FWLOGERROR("init bus node failed. ret: {}", ret);
    return ret;
  }

  ret = bus_node_->start();
  if (ret < 0) {
    FWLOGERROR("start bus node failed. ret: {}", ret);
    return ret;
  }

  // step 2. connect failed return error code
  FWLOGDEBUG("start to connect to {}", use_addr.address);
  atbus::endpoint *ep = nullptr;
  if (is_sync_channel) {
    // preallocate endpoint when using shared memory channel, because this channel can not be connected without endpoint
    std::vector<atbus::endpoint_subnet_conf> subnets;
    atbus::endpoint::ptr_t new_ep =
        atbus::endpoint::create(bus_node_.get(), conf_.id, subnets, bus_node_->get_pid(), bus_node_->get_hostname());
    ret = bus_node_->add_endpoint(new_ep);
    if (ret < 0) {
      FWLOGERROR("connect to {} failed. ret: {}", use_addr.address, ret);
      return ret;
    }

    ret = bus_node_->connect(use_addr.address.c_str(), new_ep.get());
    if (ret >= 0) {
      ep = new_ep.get();
    }
  } else {
    ret = bus_node_->connect(use_addr.address.c_str());
  }

  if (ret < 0) {
    FWLOGERROR("connect to {} failed. ret: {}", use_addr.address, ret);
    return ret;
  }

  // step 3. setup timeout timer
  bool hold_timeout_timer = setup_timeout_timer(conf_.origin.timer().initialize_timeout());
  auto timeout_timer_guard = gsl::finally([hold_timeout_timer, this]() {
    if (hold_timeout_timer) {
      this->close_timer(this->tick_timer_.timeout_timer);
    }
  });

  // step 4. waiting for connect success
  while (nullptr == ep) {
    // If there is no connection timer, it means connecting is arleady failed.
    if (0 == bus_node_->get_connection_timer_size()) {
      break;
    }

    flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
    uv_run(ev_loop, UV_RUN_ONCE);

    if (check_flag(flag_t::TIMEOUT)) {
      break;
    }
    ep = bus_node_->get_endpoint(conf_.id);
  }

  if (nullptr == ep) {
    FWLOGERROR("connect to {} failed or timeout.", use_addr.address);
    return EN_ATAPP_ERR_CONNECT_ATAPP_FAILED;
  }
  FWLOGDEBUG("connect to {} success", use_addr.address);

  flag_guard_t running_guard(*this, flag_t::RUNNING);

  // step 5. send data
  std::vector<const void *> arr_buff;
  std::vector<size_t> arr_size;
  arr_buff.resize(last_command_.size());
  arr_size.resize(last_command_.size());
  for (size_t i = 0; i < last_command_.size(); ++i) {
    arr_buff[i] = last_command_[i].data();
    arr_size[i] = last_command_[i].size();
  }

  bus_node_->set_on_custom_rsp_handle([this](const atbus::node &n, const atbus::endpoint *response_ep,
                                             const atbus::connection *conn, atbus::node::bus_id_t bus_id,
                                             const std::vector<std::pair<const void *, size_t>> &response_body,
                                             uint64_t sequence) -> int {
    return this->bus_evt_callback_on_custom_command_response(n, response_ep, conn, bus_id, response_body, sequence);
  });

  FWLOGDEBUG("send command message to {:#x} with address = {}", ep->get_id(), use_addr.address);
  ret = bus_node_->send_custom_cmd(ep->get_id(), &arr_buff[0], &arr_size[0], last_command_.size());
  if (ret < 0) {
    FWLOGERROR("send command failed. ret: {}", ret);
    return ret;
  }

  // step 6. waiting for send done(for shm, no need to wait, for io_stream fd, waiting write callback)
  if (!is_sync_channel) {
    uint64_t before_count = stats_.receive_custom_command_reponse_count;
    do {
      if (before_count != stats_.receive_custom_command_reponse_count) {
        break;
      }

      flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
      uv_run(ev_loop, UV_RUN_ONCE);
      if (check_flag(flag_t::TIMEOUT)) {
        FWLOGERROR("send command or receive response timeout");
        ret = -1;
        break;
      }

      ep = bus_node_->get_endpoint(conf_.id);
      if (nullptr == ep) {
        FWLOGERROR("send command but endpoint is reset without any response");
        break;
      }

      if (nullptr == bus_node_->get_self_endpoint()) {
        FWLOGERROR("send command but closing without any response");
        break;
      }

      if (nullptr == bus_node_->get_self_endpoint()->get_data_connection(ep)) {
        FWLOGERROR("send command but connection is reset without any response");
        break;
      }
    } while (true);
  }

  if (bus_node_) {
    bus_node_->reset();
    bus_node_.reset();
  }
  return ret;
}
}  // namespace atapp
