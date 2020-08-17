#include <sstream>
#include <vector>

#include <config/compiler/protobuf_prefix.h>

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <config/compiler/protobuf_suffix.h>

#include <common/string_oprs.h>
#include <random/random_generator.h>

#include <atframe/atapp.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include "atframe/modules/etcd_module.h"

#define ETCD_MODULE_STARTUP_RETRY_TIMES 5
#define ETCD_MODULE_BY_ID_DIR           "by_id"
#define ETCD_MODULE_BY_NAME_DIR         "by_name"
#define ETCD_MODULE_BY_TYPE_ID_DIR      "by_type_id"
#define ETCD_MODULE_BY_TYPE_NAME_DIR    "by_type_name"
#define ETCD_MODULE_BY_TAG_DIR          "by_tag"

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

        static void init_timer_timeout_callback(uv_timer_t *handle) {
            assert(handle);
            assert(handle->data);
            assert(handle->loop);

            bool *is_timeout = reinterpret_cast<bool *>(handle->data);
            *is_timeout      = true;
            uv_stop(handle->loop);
        }

        static void init_timer_tick_callback(uv_timer_t *handle) {
            if (NULL == handle->data) {
                return;
            }
            assert(handle);
            assert(handle->loop);

            etcd_cluster *cluster = reinterpret_cast<etcd_cluster *>(handle->data);
            cluster->tick();
        }

        static void init_timer_closed_callback(uv_handle_t *handle) {
            if (NULL == handle->data) {
                uv_stop(handle->loop);
                return;
            }
            assert(handle);
            assert(handle->loop);

            handle->data = NULL;
            uv_stop(handle->loop);
        }

    } // namespace detail

    LIBATAPP_MACRO_API etcd_module::etcd_module() : etcd_ctx_enabled_(false) {
        tick_next_timepoint_ = util::time::time_utility::now();
        tick_interval_ = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(128));
    }

    LIBATAPP_MACRO_API etcd_module::~etcd_module() { reset(); }

    LIBATAPP_MACRO_API void etcd_module::reset() {
        conf_path_cache_.clear();
        custom_data_.clear();
        if (cleanup_request_) {
            cleanup_request_->set_priv_data(NULL);
            cleanup_request_->set_on_complete(NULL);
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
            FWLOGERROR("init cURL failed, errcode: {}", res);
            return -1;
        }

        util::network::http_request::create_curl_multi(get_app()->get_bus_node()->get_evloop(), curl_multi_);
        if (!curl_multi_) {
            FWLOGERROR("create curl multi instance failed.");
            return -1;
        }

        const atapp::protocol::atapp_etcd &conf = get_configure();
        if (etcd_ctx_.get_conf_hosts().empty() || !etcd_ctx_enabled_) {
            FWLOGINFO("etcd host not found, start single mode");
            return res;
        }

        etcd_ctx_.init(curl_multi_);

        // generate keepalives
        std::vector<atapp::etcd_keepalive::ptr_t> keepalive_actors;
        std::string keepalive_val;
        res = init_keepalives(keepalive_actors, keepalive_val);
        if (res < 0) {
            return res;
        }
        // generate watchers data
        res = init_watchers();
        if (res < 0) {
            return res;
        }

        // Setup for first, we must check if all resource available.
        bool is_failed  = false;
        bool is_timeout = false;

        // setup timer for timeout
        uv_timer_t timeout_timer;
        uv_timer_t tick_timer;
        uv_timer_init(get_app()->get_bus_node()->get_evloop(), &timeout_timer);
        uv_timer_init(get_app()->get_bus_node()->get_evloop(), &tick_timer);
        timeout_timer.data = &is_timeout;
        tick_timer.data    = &etcd_ctx_;

        uint64_t timeout_ms = detail::convert_to_ms<uint64_t>(conf.init().timeout(), 5000);
        uv_timer_start(&timeout_timer, detail::init_timer_timeout_callback, timeout_ms, 0);
        uv_timer_start(&tick_timer, detail::init_timer_tick_callback, 128, 128);

        int ticks = 0;
        while (false == is_failed && false == is_timeout) {
            util::time::time_utility::update();
            etcd_ctx_.tick();
            ++ticks;

            size_t run_count = 0;
            // Check keepalives
            for (size_t i = 0; false == is_failed && i < keepalive_actors.size(); ++i) {
                if (keepalive_actors[i]->is_check_run()) {
                    if (!keepalive_actors[i]->is_check_passed()) {
                        FWLOGERROR("etcd_keepalive lock {} failed.", keepalive_actors[i]->get_path());
                        is_failed = true;
                    }

                    ++run_count;
                }
            }

            // Check watchers

            // 全部成功或任意失败则退出
            if (is_failed || run_count >= keepalive_actors.size()) {
                break;
            }

            uv_run(get_app()->get_bus_node()->get_evloop(), UV_RUN_ONCE);

            // 任意重试次数过多则失败退出
            for (size_t i = 0; false == is_failed && i < keepalive_actors.size(); ++i) {
                if (keepalive_actors[i]->get_check_times() >= ETCD_MODULE_STARTUP_RETRY_TIMES ||
                    etcd_ctx_.get_stats().continue_error_requests > ETCD_MODULE_STARTUP_RETRY_TIMES) {
                    size_t retry_times = keepalive_actors[i]->get_check_times();
                    if (etcd_ctx_.get_stats().continue_error_requests > retry_times) {
                        retry_times = etcd_ctx_.get_stats().continue_error_requests > retry_times;
                    }
                    FWLOGERROR("etcd_keepalive request {} for {} times (with {} ticks) failed.", keepalive_actors[i]->get_path(),
                               retry_times, ticks);
                    is_failed = true;
                    break;
                }
            }

            if (is_failed) {
                break;
            }
        }

        if (is_timeout) {
            is_failed = true;
            for (size_t i = 0; i < keepalive_actors.size(); ++i) {
                size_t retry_times = keepalive_actors[i]->get_check_times();
                if (etcd_ctx_.get_stats().continue_error_requests > retry_times) {
                    retry_times = etcd_ctx_.get_stats().continue_error_requests;
                }
                FWLOGERROR("etcd_keepalive request {} timeout, retry {} times (with {} ticks).", keepalive_actors[i]->get_path(),
                           retry_times, ticks);
            }
        }

        // close timer for timeout
        uv_timer_stop(&timeout_timer);
        uv_timer_stop(&tick_timer);
        uv_close((uv_handle_t *)&timeout_timer, detail::init_timer_closed_callback);
        uv_close((uv_handle_t *)&tick_timer, detail::init_timer_closed_callback);
        while (timeout_timer.data || tick_timer.data) {
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

    int etcd_module::init_keepalives(std::vector<atapp::etcd_keepalive::ptr_t> &keepalive_actors, std::string &keepalive_val) {
        const atapp::protocol::atapp_etcd &conf = get_configure();
        keepalive_actors.reserve(5);

        if (conf.report_alive().by_id()) {
            atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(keepalive_val, get_by_id_path());
            if (!actor) {
                FWLOGERROR("create etcd_keepalive for by_id index failed.");
                return -1;
            }

            keepalive_actors.push_back(actor);
            FWLOGINFO("create etcd_keepalive for by_id index {} success", get_by_id_path());
        }

        if (conf.report_alive().by_type()) {
            atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(keepalive_val, get_by_type_id_path());
            if (!actor) {
                FWLOGERROR("create etcd_keepalive for by_type_id index failed.");
                return -1;
            }
            keepalive_actors.push_back(actor);
            FWLOGINFO("create etcd_keepalive for by_type_id index {} success", get_by_type_id_path());

            actor = add_keepalive_actor(keepalive_val, get_by_type_name_path());
            if (!actor) {
                FWLOGERROR("create etcd_keepalive for by_type_name index failed.");
                return -1;
            }
            keepalive_actors.push_back(actor);

            FWLOGINFO("create etcd_keepalive for by_type_name index {} success", get_by_type_name_path());
        }

        if (conf.report_alive().by_name()) {
            atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(keepalive_val, get_by_name_path());
            if (!actor) {
                FWLOGERROR("create etcd_keepalive for by_name index failed.");
                return -1;
            }

            keepalive_actors.push_back(actor);
            FWLOGINFO("create etcd_keepalive for by_name index {} success", get_by_name_path());
        }

        for (int i = 0; i < conf.report_alive().by_tag_size(); ++i) {
            const std::string &tag_name = conf.report_alive().by_tag(i);

            if (tag_name.empty()) {
                continue;
            }

            atapp::etcd_keepalive::ptr_t actor = add_keepalive_actor(keepalive_val, get_by_tag_path(tag_name));
            if (!actor) {
                FWLOGERROR("create etcd_keepalive for by_tag {} index failed.", tag_name);
                return -1;
            }

            keepalive_actors.push_back(actor);
            FWLOGINFO("create etcd_keepalive for by_tag index {} success", get_by_tag_path(tag_name));
        }

        // TODO create custom keepalives
        return 0;
    }

    int etcd_module::init_watchers() {
        // TODO setup configured watchers
        // TODO 内置有按id或按name watch, return
        // if (!watcher_by_name_callbacks_.empty()) {}
        // if (!watcher_by_id_callbacks_.empty()) {}
        // TODO 内置有按type id或按type name watch
        // TODO 内置有按tag watch
        // TODO 内置有自定义 watch

        // TODO create custom watchers
        return 0;
    }

    LIBATAPP_MACRO_API int etcd_module::reload() {
        // load configure
        const atapp::protocol::atapp_etcd &conf = get_configure();
        conf_path_cache_.clear();

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
        etcd_ctx_.set_conf_http_timeout(detail::convert_to_chrono(conf.request().timeout(), 10000));
        etcd_ctx_.set_conf_etcd_members_update_interval(detail::convert_to_chrono(conf.cluster().update_interval(), 300000));
        etcd_ctx_.set_conf_etcd_members_retry_interval(detail::convert_to_chrono(conf.cluster().retry_interval(), 60000));

        etcd_ctx_.set_conf_keepalive_timeout(detail::convert_to_chrono(conf.keepalive().timeout(), 16000));
        etcd_ctx_.set_conf_keepalive_interval(detail::convert_to_chrono(conf.keepalive().ttl(), 5000));

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
            std::chrono::seconds(conf.init().tick_interval().seconds()) + std::chrono::nanoseconds(conf.init().tick_interval().nanos())
        );
        if (tick_interval_ < std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(32))) {
            tick_interval_ = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::milliseconds(128));
        }
        return 0;
    }

    LIBATAPP_MACRO_API int etcd_module::stop() {
        if (!cleanup_request_) {
            cleanup_request_ = etcd_ctx_.close(false);

            if (cleanup_request_ && cleanup_request_->is_running()) {
                cleanup_request_->set_priv_data(this);
                cleanup_request_->set_on_complete(http_callback_on_etcd_closed);
            }
        }

        if (cleanup_request_) {
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
        // Slow down the tick interval of etcd module, it require http request which is very slow compared to atbus
        if (tick_next_timepoint_ >= util::time::time_utility::now()) {
            return 0;
        }
        tick_next_timepoint_ = util::time::time_utility::now() + tick_interval_;

        // single mode
        if (etcd_ctx_.get_conf_hosts().empty() || !etcd_ctx_enabled_) {
            // If it's initializing, is_available() will return false
            if (!cleanup_request_ && etcd_ctx_.is_available()) {
                cleanup_request_ = etcd_ctx_.close(false);
            }

            return 0;
        }

        // previous closing not finished, wait for it
        if (cleanup_request_) {
            if (cleanup_request_->is_running()) {
                return 0;
            } else {
                cleanup_request_.reset();
            }
        }

        // first startup when reloaded
        if (!curl_multi_) {
            int res = init();
            if (res < 0) {
                FWLOGERROR("initialize etcd failed, res: {}", res);
                get_app()->stop();
                return res;
            }
        } else if (etcd_ctx_.check_flag(etcd_cluster::flag_t::CLOSING)) { // Already stoped and restart etcd_ctx_
            etcd_ctx_.init(curl_multi_);
             // generate keepalives
            std::vector<atapp::etcd_keepalive::ptr_t> keepalive_actors;
            std::string keepalive_val;
            int res = init_keepalives(keepalive_actors, keepalive_val);
            if (res < 0) {
                FWLOGERROR("reinitialize etcd keepalives failed, res: {}", res);
            }
            // generate watchers data
            res = init_watchers();
            if (res < 0) {
                FWLOGERROR("reinitialize etcd watchers failed, res: {}", res);
            }
        }

        return etcd_ctx_.tick();
    }

    LIBATAPP_MACRO_API const std::string &etcd_module::get_conf_custom_data() const { return custom_data_; }
    LIBATAPP_MACRO_API void etcd_module::set_conf_custom_data(const std::string &v) { custom_data_ = v; }

    LIBATAPP_MACRO_API bool etcd_module::is_etcd_enabled() const { return etcd_ctx_enabled_ && etcd_ctx_.get_conf_hosts().empty(); }
    LIBATAPP_MACRO_API void etcd_module::enable_etcd() { etcd_ctx_enabled_ = true; }
    LIBATAPP_MACRO_API void etcd_module::disable_etcd() { etcd_ctx_enabled_ = false; }

    LIBATAPP_MACRO_API const util::network::http_request::curl_m_bind_ptr_t &etcd_module::get_shared_curl_multi_context() const {
        return curl_multi_;
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_id_path() const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_ID_DIR, get_app()->get_app_name(), get_app()->get_id());
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_type_id_path() const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_TYPE_ID_DIR, get_app()->get_type_id(),
                                        get_app()->get_app_name(), get_app()->get_id());
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_type_name_path() const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_TYPE_NAME_DIR, get_app()->get_type_name(),
                                        get_app()->get_app_name(), get_app()->get_id());
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_name_path() const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_NAME_DIR, get_app()->get_app_name(), get_app()->get_id());
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_tag_path(const std::string &tag_name) const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}/{}-{}", get_configure_path(), ETCD_MODULE_BY_TAG_DIR, tag_name, get_app()->get_app_name(), get_app()->get_id());
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_id_watcher_path() const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}", get_configure_path(), ETCD_MODULE_BY_ID_DIR);
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_type_id_watcher_path(uint64_t type_id) const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_ID_DIR, type_id);
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_type_name_watcher_path(const std::string &type_name) const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_NAME_DIR, type_name);
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_name_watcher_path() const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}", get_configure_path(), ETCD_MODULE_BY_NAME_DIR);
    }

    LIBATAPP_MACRO_API std::string etcd_module::get_by_tag_watcher_path(const std::string &tag_name) const {
        return LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}", get_configure_path(), ETCD_MODULE_BY_TAG_DIR, tag_name);
    }

    LIBATAPP_MACRO_API int etcd_module::add_watcher_by_id(watcher_list_callback_t fn) {
        if (!fn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        bool need_setup_callback = watcher_by_id_callbacks_.empty();

        if (need_setup_callback) {
            std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}/{}", get_configure_path(), ETCD_MODULE_BY_ID_DIR);

            atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
            if (!p) {
                FWLOGERROR("create etcd_watcher by_id failed.");
                return EN_ATBUS_ERR_MALLOC;
            }

            p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
            p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
            etcd_ctx_.add_watcher(p);
            FWLOGINFO("create etcd_watcher for by_id index {} success", watch_path);

            p->set_evt_handle(watcher_callback_list_wrapper_t(*this, watcher_by_id_callbacks_));
        }

        watcher_by_id_callbacks_.push_back(fn);
        return 0;
    }

    LIBATAPP_MACRO_API int etcd_module::add_watcher_by_type_id(uint64_t type_id, watcher_one_callback_t fn) {
        if (!fn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // generate watch data
        std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_ID_DIR, type_id);

        atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
        if (!p) {
            FWLOGERROR("create etcd_watcher by_type_id failed.");
            return EN_ATBUS_ERR_MALLOC;
        }

        p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
        p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
        etcd_ctx_.add_watcher(p);
        FWLOGINFO("create etcd_watcher for by_type_id index {} success", watch_path);

        p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn));
        return 0;
    }

    LIBATAPP_MACRO_API int etcd_module::add_watcher_by_type_name(const std::string &type_name, watcher_one_callback_t fn) {
        if (!fn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // generate watch data
        std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}", get_configure_path(), ETCD_MODULE_BY_TYPE_NAME_DIR, type_name);

        atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
        if (!p) {
            FWLOGERROR("create etcd_watcher by_type_name failed.");
            return EN_ATBUS_ERR_MALLOC;
        }

        p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
        p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
        etcd_ctx_.add_watcher(p);
        FWLOGINFO("create etcd_watcher for by_type_name index {} success", watch_path);

        p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn));
        return 0;
    }

    LIBATAPP_MACRO_API int etcd_module::add_watcher_by_name(watcher_list_callback_t fn) {
        if (!fn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        bool need_setup_callback = watcher_by_name_callbacks_.empty();

        if (need_setup_callback) {
            // generate watch data
            std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}/{}", get_configure_path(), ETCD_MODULE_BY_NAME_DIR);

            atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
            if (!p) {
                FWLOGERROR("create etcd_watcher by_name failed.");
                return EN_ATBUS_ERR_MALLOC;
            }

            p->set_conf_request_timeout(detail::convert_to_chrono(get_configure().watcher().request_timeout(), 3600000));
            p->set_conf_retry_interval(detail::convert_to_chrono(get_configure().watcher().retry_interval(), 15000));
            etcd_ctx_.add_watcher(p);
            FWLOGINFO("create etcd_watcher for by_name index {} success", watch_path);

            p->set_evt_handle(watcher_callback_list_wrapper_t(*this, watcher_by_name_callbacks_));
        }

        watcher_by_name_callbacks_.push_back(fn);
        return 0;
    }

    LIBATAPP_MACRO_API int etcd_module::add_watcher_by_tag(const std::string &tag_name, watcher_one_callback_t fn) {
        if (!fn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // generate watch data
        std::string watch_path = LOG_WRAPPER_FWAPI_FORMAT("{}/{}/{}", get_configure_path(), ETCD_MODULE_BY_TAG_DIR, tag_name);

        atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, watch_path, "+1");
        if (!p) {
            FWLOGERROR("create etcd_watcher by_tag failed.");
            return EN_ATBUS_ERR_MALLOC;
        }

        etcd_ctx_.add_watcher(p);
        FWLOGINFO("create etcd_watcher for by_tag index {} success", watch_path);

        p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn));
        return 0;
    }

    LIBATAPP_MACRO_API atapp::etcd_watcher::ptr_t etcd_module::add_watcher_by_custom_path(const std::string &custom_path,
                                                                                          watcher_one_callback_t fn) {
        if (!fn) {
            FWLOGERROR("create etcd_watcher by custom path {} failed. callback can not be empty.", custom_path);
            return NULL;
        }

        if (custom_path.size() < get_configure_path().size() ||
            0 != UTIL_STRFUNC_STRNCMP(custom_path.c_str(), get_configure_path().c_str(), get_configure_path().size())) {

            FWLOGERROR("create etcd_watcher by custom path {} failed. path must has prefix of {}.", custom_path, get_configure_path());
            return NULL;
        }

        atapp::etcd_watcher::ptr_t p = atapp::etcd_watcher::create(etcd_ctx_, custom_path, "+1");
        if (!p) {
            FWLOGERROR("create etcd_watcher by custom path {} failed. malloc etcd_watcher failed.", custom_path);
            return NULL;
        }

        p->set_evt_handle(watcher_callback_one_wrapper_t(*this, fn));
        if (etcd_ctx_.add_watcher(p)) {
            FWLOGINFO("create etcd_watcher by custom path {} success", custom_path);
        } else {
            FWLOGINFO("add etcd_watcher by custom path {} failed", custom_path);
            p->set_evt_handle(NULL);
            p.reset();
        }

        return p;
    }

    LIBATAPP_MACRO_API const ::atapp::etcd_cluster &etcd_module::get_raw_etcd_ctx() const { return etcd_ctx_; }
    LIBATAPP_MACRO_API ::atapp::etcd_cluster &etcd_module::get_raw_etcd_ctx() { return etcd_ctx_; }

    LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &etcd_module::get_configure() const {
        return get_app()->get_origin_configure().etcd();
    }

    LIBATAPP_MACRO_API const std::string &etcd_module::get_configure_path() const {
        if (conf_path_cache_.empty()) {
            etcd_module *self      = const_cast<etcd_module *>(this);
            self->conf_path_cache_ = get_configure().path();

            if (!self->conf_path_cache_.empty()) {
                size_t last_idx = self->conf_path_cache_.size() - 1;
                if (self->conf_path_cache_[last_idx] != '/' && self->conf_path_cache_[last_idx] != '\\') {
                    self->conf_path_cache_ += '/';
                }
            }
        }

        return conf_path_cache_;
    }

    LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t etcd_module::add_keepalive_actor(std::string &val, const std::string &node_path) {
        atapp::etcd_keepalive::ptr_t ret;
        if (val.empty()) {
            node_info_t ni;
            get_app()->pack(ni.node_discovery);
            pack(ni, val);
        }

        ret = atapp::etcd_keepalive::create(etcd_ctx_, node_path);
        if (!ret) {
            FWLOGERROR("create etcd_keepalive failed.");
            return ret;
        }

        ret->set_checker(val);
        ret->set_value(val);

        if (!etcd_ctx_.add_keepalive(ret)) {
            ret.reset();
        }

        return ret;
    }

    LIBATAPP_MACRO_API etcd_module::node_event_callback_handle_t etcd_module::add_on_node_discovery_event(node_event_callback_t fn) {
        if (!fn) {
            return node_event_callbacks_.end();
        }

        return node_event_callbacks_.insert(node_event_callbacks_.end(), fn);
    }

    LIBATAPP_MACRO_API void etcd_module::remove_on_node_event(node_event_callback_handle_t handle) {
        if (handle == node_event_callbacks_.end()) {
            return;
        }

        node_event_callbacks_.erase(handle);
    }

    bool etcd_module::unpack(node_info_t &out, const std::string &path, const std::string &json, bool reset_data) {
        if (reset_data) {
            out.node_discovery.Clear();
        }

        if (json.empty()) {
            size_t start_idx = 0;
            size_t last_minus = 0;
            for (size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '/' || path[i] == '\\' || path[i] == ' ' || path[i] == '\t' || path[i] == '\r' || path[i] == '\n') {
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

        return atapp::rapidsjon_loader_parse(out.node_discovery, json);
    }

    void etcd_module::pack(const node_info_t &src, std::string &json) { json = atapp::rapidsjon_loader_stringify(src.node_discovery); }

    int etcd_module::http_callback_on_etcd_closed(util::network::http_request &req) {
        etcd_module *self = reinterpret_cast<etcd_module *>(req.get_priv_data());
        if (NULL == self) {
            FWLOGERROR("etcd_module get request shouldn't has request without private data");
            return 0;
        }

        self->cleanup_request_.reset();

        FWLOGDEBUG("Etcd revoke lease finished");

        // call stop to trigger stop process again.
        self->get_app()->stop();

        return 0;
    }

    etcd_module::watcher_callback_list_wrapper_t::watcher_callback_list_wrapper_t(etcd_module &m, std::list<watcher_list_callback_t> &cbks)
        : mod(&m), callbacks(&cbks) {}
    void etcd_module::watcher_callback_list_wrapper_t::operator()(const ::atapp::etcd_response_header &header,
                                                                  const ::atapp::etcd_watcher::response_t &body) {
        if (NULL == mod) {
            return;
        }
        // decode data
        for (size_t i = 0; i < body.events.size(); ++i) {
            const ::atapp::etcd_watcher::event_t &evt_data = body.events[i];
            node_info_t node;
            if (evt_data.kv.value.empty()) {
                unpack(node, evt_data.kv.key, evt_data.prev_kv.value, true);
            } else {
                unpack(node, evt_data.kv.key, evt_data.kv.value, true);
            }
            if (node.node_discovery.id() == 0 && node.node_discovery.name().empty()) {
                continue;
            }

            if (evt_data.evt_type == ::atapp::etcd_watch_event::EN_WEVT_DELETE) {
                node.action = node_action_t::EN_NAT_DELETE;
            } else {
                node.action = node_action_t::EN_NAT_PUT;
            }

            mod->update_inner_watcher_event(node);

            if (NULL == callbacks) {
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
    }

    etcd_module::watcher_callback_one_wrapper_t::watcher_callback_one_wrapper_t(etcd_module &m, watcher_one_callback_t cbk)
        : mod(&m), callback(cbk) {}
    void etcd_module::watcher_callback_one_wrapper_t::operator()(const ::atapp::etcd_response_header &header,
                                                                 const ::atapp::etcd_watcher::response_t &body) {
        if (NULL == mod) {
            return;
        }

        // decode data
        for (size_t i = 0; i < body.events.size(); ++i) {
            const ::atapp::etcd_watcher::event_t &evt_data = body.events[i];
            node_info_t node;

            if (evt_data.kv.value.empty()) {
                unpack(node, evt_data.kv.key, evt_data.prev_kv.value, true);
            } else {
                unpack(node, evt_data.kv.key, evt_data.kv.value, true);
            }
            if (node.node_discovery.id() == 0 && node.node_discovery.name().empty()) {
                continue;
            }

            if (evt_data.evt_type == ::atapp::etcd_watch_event::EN_WEVT_DELETE) {
                node.action = node_action_t::EN_NAT_DELETE;
            } else {
                node.action = node_action_t::EN_NAT_PUT;
            }

            mod->update_inner_watcher_event(node);

            if (NULL == callback) {
                continue;
            }
            watcher_sender_one_t sender(*mod, header, body, evt_data, node);
            callback(std::ref(sender));
        }
    }

    bool etcd_module::update_inner_watcher_event(node_info_t& node) {
        atapp_discovery_ptr_t local_cache_by_id;
        atapp_discovery_ptr_t local_cache_by_name;
        if (node.node_discovery.id() != 0) {
            node_discovery_cache_by_id_t::iterator iter = node_discovery_cache_by_id_.find(node.node_discovery.id());
            if (iter != node_discovery_cache_by_id_.end()) {
                local_cache_by_id = iter->second;
            }
        }

        if (!node.node_discovery.name().empty()) {
            node_discovery_cache_by_name_t::iterator iter = node_discovery_cache_by_name_.find(node.node_discovery.name());
            if (iter != node_discovery_cache_by_name_.end()) {
                local_cache_by_name = iter->second;
            }
        }

        bool has_event = false;

        if (likely(local_cache_by_id == local_cache_by_name)) {
            if (node_action_t::EN_NAT_DELETE == node.action) {
                if (local_cache_by_id) {
                    node_discovery_cache_by_id_.erase(node.node_discovery.id());
                    node_discovery_cache_by_name_.erase(node.node_discovery.name());

                    has_event = true;
                }
            } else {
                if (local_cache_by_id && false == protobuf_equal(*local_cache_by_id, node.node_discovery)) {
                    return false;
                }

                atapp_discovery_ptr_t new_inst = std::make_shared<atapp::protocol::atapp_discovery>();
                new_inst->CopyFrom(node.node_discovery);
                node_discovery_cache_by_id_[node.node_discovery.id()] = new_inst;
                node_discovery_cache_by_name_[node.node_discovery.name()] = new_inst;

                has_event = true;
            }
        } else {
            if (node_action_t::EN_NAT_DELETE == node.action) {
                if (local_cache_by_id) {
                    node_discovery_cache_by_id_.erase(node.node_discovery.id());
                    has_event = true;
                }
                if (local_cache_by_name) {
                    node_discovery_cache_by_name_.erase(node.node_discovery.name());
                    has_event = true;
                }
            } else {
                atapp_discovery_ptr_t new_inst;
                if (!(local_cache_by_id && false == protobuf_equal(*local_cache_by_id, node.node_discovery))) {
                    new_inst = std::make_shared<atapp::protocol::atapp_discovery>();
                    new_inst->CopyFrom(node.node_discovery);

                    node_discovery_cache_by_id_[node.node_discovery.id()] = new_inst;
                    has_event = true;
                }

                if (!(local_cache_by_name && false == protobuf_equal(*local_cache_by_name, node.node_discovery))) {
                    if (!new_inst) {
                        new_inst = std::make_shared<atapp::protocol::atapp_discovery>();
                        new_inst->CopyFrom(node.node_discovery);
                    }

                    node_discovery_cache_by_name_[node.node_discovery.name()] = new_inst;
                    has_event = true;
                }
            }

            if (has_event) {
                for (node_event_callback_list_t::iterator iter = node_event_callbacks_.begin(); iter != node_event_callbacks_.end(); ++ iter) {
                    if (*iter) {
                        (*iter)(node);
                    }
                }
            }
        }

        return has_event;
    }

} // namespace atapp
