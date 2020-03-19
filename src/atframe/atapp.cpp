#include <assert.h>
#include <signal.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>


#include "std/foreach.h"
#include "std/static_assert.h"

#include "atframe/atapp.h"

#include "libatbus.h"
#include "libatbus_protocol.h"

#include <algorithm/murmur_hash.h>
#include <common/file_system.h>
#include <common/string_oprs.h>

#include "cli/shell_font.h"


namespace atapp {
    app *app::last_instance_;

    LIBATAPP_MACRO_API app::flag_guard_t::flag_guard_t(app &owner, flag_t::type f) : owner_(&owner), flag_(f) {
        if (owner_->check_flag(flag_)) {
            owner_ = NULL;
            return;
        }

        owner_->set_flag(flag_, true);
    }

    LIBATAPP_MACRO_API app::flag_guard_t::~flag_guard_t() {
        if (NULL == owner_) {
            return;
        }

        owner_->set_flag(flag_, false);
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

    LIBATAPP_MACRO_API app::app() : setup_result_(0), last_proc_event_count_(0), mode_(mode_t::CUSTOM) {
        last_instance_ = this;
        conf_.id = 0;
        conf_.type_id = 0;
        conf_.execute_path = NULL;
        conf_.resume_mode = false;
        conf_.remove_pidfile_after_exit = true;
        conf_.stop_timeout = 30000; // 30s
        conf_.tick_interval = 32;   // 32ms

        tick_timer_.sec_update = util::time::time_utility::raw_time_t::min();
        tick_timer_.sec = 0;
        tick_timer_.usec = 0;

        tick_timer_.tick_timer.is_activited = false;
        tick_timer_.timeout_timer.is_activited = false;

        stat_.last_checkpoint_min = 0;
    }

    LIBATAPP_MACRO_API app::~app() {
        if (this == last_instance_) {
            last_instance_ = NULL;
        }

        owent_foreach(module_ptr_t & mod, modules_) {
            if (mod && mod->owner_ == this) {
                mod->owner_ = NULL;
            }
        }

        // reset atbus first, make sure atbus ref count is greater than 0 when reset it
        // some inner async deallocate action will add ref count and we should make sure
        // atbus is not destroying
        if (bus_node_) {
            bus_node_->reset();
            bus_node_.reset();
        }

        assert(!tick_timer_.tick_timer.is_activited);
        assert(!tick_timer_.timeout_timer.is_activited);
    }

    LIBATAPP_MACRO_API int app::run(uv_loop_t *ev_loop, int argc, const char **argv, void *priv_data) {
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

    } // namespace atapp

    LIBATAPP_MACRO_API int app::init(uv_loop_t *ev_loop, int argc, const char **argv, void *priv_data) {
        if (check_flag(flag_t::INITIALIZED)) {
            return EN_ATAPP_ERR_ALREADY_INITED;
        }
        setup_result_ = 0;

        if (check_flag(flag_t::IN_CALLBACK)) {
            return 0;
        }

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

        util::cli::shell_stream ss(std::cerr);
        // step 4. load options from cmd line
        conf_.bus_conf.ev_loop = ev_loop;
        int ret = reload();
        if (ret < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "load configure failed" << std::endl;
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
        default: { return setup_result_ = 0; }
        }

        // step 6. setup log & signal
        ret = setup_log();
        if (ret < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "setup log failed" << std::endl;
            write_pidfile();
            return setup_result_ = ret;
        }

        ret = setup_signal();
        if (ret < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "setup signal failed" << std::endl;
            write_pidfile();
            return setup_result_ = ret;
        }

        ret = setup_atbus();
        if (ret < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "setup atbus failed" << std::endl;
            bus_node_.reset();
            write_pidfile();
            return setup_result_ = ret;
        }

        // step 7. all modules reload
        owent_foreach(module_ptr_t & mod, modules_) {
            if (mod->is_enabled()) {
                ret = mod->reload();
                if (ret < 0) {
                    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "load configure of " << mod->name() << " failed" << std::endl;
                    write_pidfile();
                    return setup_result_ = ret;
                }
            }
        }

        // step 8. all modules init
        size_t inited_mod_idx = 0;
        int mod_init_res = 0;
        for (; mod_init_res >= 0 && inited_mod_idx < modules_.size(); ++inited_mod_idx) {
            if (modules_[inited_mod_idx]->is_enabled()) {
                mod_init_res = modules_[inited_mod_idx]->init();
                if (mod_init_res < 0) {
                    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "initialze " << modules_[inited_mod_idx]->name() << " failed" << std::endl;
                    break;
                }
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
            write_pidfile();
            return setup_result_ = mod_init_res;
        }

        // callback of all modules inited
        if (evt_on_all_module_inited_) {
            evt_on_all_module_inited_(*this);
        }

        // step 9. write pid file
        if (false == write_pidfile()) {
            return EN_ATAPP_ERR_WRITE_PID_FILE;
        }

        if (setup_timer() < 0) {
            // cleanup modules
            for (std::vector<module_ptr_t>::reverse_iterator rit = modules_.rbegin(); rit != modules_.rend(); ++rit) {
                if (*rit) {
                    (*rit)->cleanup();
                }
            }

            return EN_ATAPP_ERR_SETUP_TIMER;
        }

        set_flag(flag_t::STOPPED, false);
        set_flag(flag_t::STOPING, false);
        set_flag(flag_t::INITIALIZED, true);
        set_flag(flag_t::RUNNING, true);

        return EN_ATAPP_ERR_SUCCESS;
    } // namespace atapp

    LIBATAPP_MACRO_API int app::run_noblock(uint64_t max_event_count) {
        uint64_t evt_count = 0;
        int ret = 0;
        do {
            ret = run_inner(UV_RUN_NOWAIT);
            if (ret < 0) {
                break;
            }

            if (0 == last_proc_event_count_) {
                break;
            }

            evt_count += last_proc_event_count_;
        } while (0 == max_event_count || evt_count < max_event_count);

        return ret;
    }

    LIBATAPP_MACRO_API bool app::is_inited() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::INITIALIZED); }

    LIBATAPP_MACRO_API bool app::is_running() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::RUNNING); }

    LIBATAPP_MACRO_API bool app::is_closing() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::STOPING); }

    LIBATAPP_MACRO_API bool app::is_closed() const UTIL_CONFIG_NOEXCEPT { return check_flag(flag_t::STOPPED); }

    LIBATAPP_MACRO_API int app::reload() {
        app_conf old_conf = conf_;
        util::cli::shell_stream ss(std::cerr);

        WLOGINFO("============ start to load configure ============");
        // step 1. reset configure
        cfg_loader_.clear();

        // step 2. reload from program configure file
        if (conf_.conf_file.empty()) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "missing configure file" << std::endl;
            print_help();
            return EN_ATAPP_ERR_MISSING_CONFIGURE_FILE;
        }
        if (cfg_loader_.load_file(conf_.conf_file.c_str(), false) < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "load configure file " << conf_.conf_file << " failed" << std::endl;
            print_help();
            return EN_ATAPP_ERR_LOAD_CONFIGURE_FILE;
        }

        // step 3. reload from external configure files
        // step 4. merge configure
        {
            std::vector<std::string> external_confs;
            cfg_loader_.dump_to("atapp.config.external", external_confs);
            owent_foreach(std::string & conf_fp, external_confs) {
                if (!conf_fp.empty()) {
                    if (cfg_loader_.load_file(conf_.conf_file.c_str(), true) < 0) {
                        if (is_running()) {
                            WLOGERROR("load external configure file %s failed", conf_fp.c_str());
                        } else {
                            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "load external configure file " << conf_fp << " failed" << std::endl;
                            return 1;
                        }
                    }
                }
            }
        }

        // apply ini configure
        apply_configure();
        // reuse ev loop if not configued
        if (NULL == conf_.bus_conf.ev_loop) {
            conf_.bus_conf.ev_loop = old_conf.bus_conf.ev_loop;
        }

        // step 5. if not in start mode, return
        if (mode_t::START != mode_) {
            return 0;
        }

        if (is_running()) {
            // step 6. reset log
            setup_log();

            // step 7. if inited, let all modules reload
            owent_foreach(module_ptr_t & mod, modules_) {
                if (mod->is_enabled()) {
                    mod->reload();
                }
            }
        }

        // step 8. if running and tick interval changed, reset timer
        if (old_conf.tick_interval != conf_.tick_interval) {
            set_flag(flag_t::RESET_TIMER, true);

            if (is_running()) {
                uv_stop(bus_node_->get_evloop());
            }
        }

        WLOGINFO("------------ load configure done ------------");
        return 0;
    }

    LIBATAPP_MACRO_API int app::stop() {
        WLOGINFO("============ receive stop signal and ready to stop all modules ============");
        // step 1. set stop flag.
        // bool is_stoping = set_flag(flag_t::STOPING, true);
        set_flag(flag_t::STOPING, true);

        // TODO stop reason = manual stop
        if (bus_node_ && ::atbus::node::state_t::CREATED != bus_node_->get_state() && !bus_node_->check_flag(::atbus::node::flag_t::EN_FT_SHUTDOWN)) {
            bus_node_->shutdown(0);
        }

        // step 2. stop libuv and return from uv_run
        // if (!is_stoping) {
        if (bus_node_ && NULL != bus_node_->get_evloop()) {
            uv_stop(bus_node_->get_evloop());
        }
        // }
        return 0;
    }

    LIBATAPP_MACRO_API int app::tick() {
        int active_count;
        util::time::time_utility::update();
        // record start time point
        util::time::time_utility::raw_time_t start_tp = util::time::time_utility::now();
        util::time::time_utility::raw_time_t end_tp = start_tp;
        do {
            if (tick_timer_.sec != util::time::time_utility::get_now()) {
                tick_timer_.sec = util::time::time_utility::get_now();
                tick_timer_.usec = 0;
                tick_timer_.sec_update = util::time::time_utility::now();
            } else {
                tick_timer_.usec = static_cast<time_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(util::time::time_utility::now() - tick_timer_.sec_update).count());
            }

            active_count = 0;
            int res;
            // step 1. proc available modules
            owent_foreach(module_ptr_t & mod, modules_) {
                if (mod->is_enabled()) {
                    res = mod->tick();
                    if (res < 0) {
                        WLOGERROR("module %s run tick and return %d", mod->name(), res);
                    } else {
                        active_count += res;
                    }
                }
            }

            // step 2. proc atbus
            if (bus_node_ && ::atbus::node::state_t::CREATED != bus_node_->get_state()) {
                res = bus_node_->proc(tick_timer_.sec, tick_timer_.usec);
                if (res < 0) {
                    WLOGERROR("atbus run tick and return %d", res);
                } else {
                    active_count += res;
                }
            }

            // only tick time less than tick interval will run loop again
            util::time::time_utility::update();
            end_tp = util::time::time_utility::now();

            if (active_count > 0) {
                last_proc_event_count_ += static_cast<uint64_t>(active_count);
            }
        } while (active_count > 0 && (end_tp - start_tp) < std::chrono::milliseconds(conf_.tick_interval));

        // if is stoping, quit loop  every tick
        if (check_flag(flag_t::STOPING) && bus_node_ && NULL != bus_node_->get_evloop()) {
            uv_stop(bus_node_->get_evloop());
        }

        // stat log
        do {
            time_t now_min = util::time::time_utility::get_now() / util::time::time_utility::MINITE_SECONDS;
            if (now_min != stat_.last_checkpoint_min) {
                time_t last_min = stat_.last_checkpoint_min;
                stat_.last_checkpoint_min = now_min;
                if (last_min + 1 == now_min) {
                    uv_rusage_t last_usage;
                    memcpy(&last_usage, &stat_.last_checkpoint_usage, sizeof(uv_rusage_t));
                    if (0 != uv_getrusage(&stat_.last_checkpoint_usage)) {
                        break;
                    }
                    long offset_usr = stat_.last_checkpoint_usage.ru_utime.tv_sec - last_usage.ru_utime.tv_sec;
                    long offset_sys = stat_.last_checkpoint_usage.ru_stime.tv_sec - last_usage.ru_stime.tv_sec;
                    offset_usr *= 1000000;
                    offset_sys *= 1000000;
                    offset_usr += stat_.last_checkpoint_usage.ru_utime.tv_usec - last_usage.ru_utime.tv_usec;
                    offset_sys += stat_.last_checkpoint_usage.ru_stime.tv_usec - last_usage.ru_stime.tv_usec;

                    std::pair<uint64_t, const char *> max_rss = make_size_showup(last_usage.ru_maxrss);
#ifdef WIN32
                    WLOGINFO("[STAT]: %s CPU usage: user %02.03f%%, sys %02.03f%%, max rss: %llu%s, page faults: %llu", get_app_name().c_str(),
                             offset_usr / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f), // usec and add %
                             offset_sys / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f), // usec and add %
                             static_cast<unsigned long long>(max_rss.first), max_rss.second, static_cast<unsigned long long>(last_usage.ru_majflt));
#else
                    std::pair<uint64_t, const char *> ru_ixrss = make_size_showup(last_usage.ru_ixrss);
                    std::pair<uint64_t, const char *> ru_idrss = make_size_showup(last_usage.ru_idrss);
                    std::pair<uint64_t, const char *> ru_isrss = make_size_showup(last_usage.ru_isrss);
                    WLOGINFO("[STAT]: %s CPU usage: user %02.03f%%, sys %02.03f%%, max rss: %llu%s, shared size: %llu%s, unshared data size: %llu%s, unshared "
                             "stack size: %llu%s, page faults: %llu",
                             get_app_name().c_str(),
                             offset_usr / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f), // usec and add %
                             offset_sys / (static_cast<float>(util::time::time_utility::MINITE_SECONDS) * 10000.0f), // usec and add %
                             static_cast<unsigned long long>(max_rss.first), max_rss.second, static_cast<unsigned long long>(ru_ixrss.first), ru_ixrss.second,
                             static_cast<unsigned long long>(ru_idrss.first), ru_idrss.second, static_cast<unsigned long long>(ru_isrss.first), ru_isrss.second,
                             static_cast<unsigned long long>(last_usage.ru_majflt));
#endif
                } else {
                    uv_getrusage(&stat_.last_checkpoint_usage);
                }

                if (bus_node_ && NULL != bus_node_->get_evloop()) {
                    uv_stop(bus_node_->get_evloop());
                }
            }
        } while (false);
        return 0;
    }

    LIBATAPP_MACRO_API app::app_id_t app::get_id() const { return conf_.id; }

    LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in) const { return convert_app_id_by_string(id_in, conf_.id_mask); }

    LIBATAPP_MACRO_API void app::add_module(module_ptr_t module) {
        if (this == module->owner_) {
            return;
        }

        assert(NULL == module->owner_);

        modules_.push_back(module);
        module->owner_ = this;
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

    LIBATAPP_MACRO_API void app::set_app_version(const std::string &ver) { conf_.app_version = ver; }

    LIBATAPP_MACRO_API const std::string &app::get_app_version() const { return conf_.app_version; }

    LIBATAPP_MACRO_API void app::set_build_version(const std::string &ver) { build_version_ = ver; }

    LIBATAPP_MACRO_API const std::string &app::get_build_version() const {
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
#ifdef __TIME__
            ss << " " << __TIME__;
#endif
            ss << std::endl;
#endif


#if defined(PROJECT_SCM_VERSION) || defined(PROJECT_SCM_NAME) || defined(PROJECT_SCM_BRANCH)
            ss << std::setw(key_padding) << "Build SCM:";
#ifdef PROJECT_SCM_NAME
            ss << " " << PROJECT_SCM_NAME;
#endif
#ifdef PROJECT_SCM_BRANCH
            ss << " branch " << PROJECT_SCM_BRANCH;
#endif
#ifdef PROJECT_SCM_VERSION
            ss << " commit " << PROJECT_SCM_VERSION;
#endif
#endif

#if defined(_MSC_VER)
            ss << std::setw(key_padding) << "Build Compiler: MSVC ";
#ifdef _MSC_FULL_VER
            ss << _MSC_FULL_VER;
#else
            ss << _MSC_VER;
#endif

#ifdef _MSVC_LANG
            ss << " with standard " << _MSVC_LANG;
#endif
            ss << std::endl;

#elif defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
            ss << std::setw(key_padding) << "Build Compiler: ";
#if defined(__clang__)
            ss << "clang ";
#else
            ss << "gcc ";
#endif

#if defined(__clang_version__)
            ss << __clang_version__;
#elif defined(__VERSION__)
            ss << __VERSION__;
#endif
#if defined(__OPTIMIZE__)
            ss << " optimize level " << __OPTIMIZE__;
#endif
#if defined(__STDC_VERSION__)
            ss << " C standard " << __STDC_VERSION__;
#endif
#if defined(__cplusplus) && __cplusplus > 1
            ss << " C++ standard " << __cplusplus;
#endif
            ss << std::endl;
#endif

            build_version_ = ss.str();
        }

        return build_version_;
    }

    LIBATAPP_MACRO_API const std::string &app::get_app_name() const { return conf_.name; }

    LIBATAPP_MACRO_API const std::string &app::get_type_name() const { return conf_.type_name; }

    LIBATAPP_MACRO_API app::app_id_t app::get_type_id() const { return conf_.type_id; }

    LIBATAPP_MACRO_API const std::string &app::get_hash_code() const { return conf_.hash_code; }

    LIBATAPP_MACRO_API std::shared_ptr<atbus::node> app::get_bus_node() { return bus_node_; }
    LIBATAPP_MACRO_API const std::shared_ptr<atbus::node> app::get_bus_node() const { return bus_node_; }

    LIBATAPP_MACRO_API bool app::is_remote_address_available(const std::string & /*hostname*/, const std::string &address) const {
        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem:", address.c_str(), 4)) {
            return false;
        }

        if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm:", address.c_str(), 4) || 0 == UTIL_STRFUNC_STRNCASE_CMP("unix:", address.c_str(), 5)) {
            // return hostname == ::atbus::node::get_hostname();
            // shm can not used as a remote address, it can only connect with a exist endpoint
            return false;
        }

        return true;
    }

    LIBATAPP_MACRO_API util::config::ini_loader &app::get_configure() { return cfg_loader_; }
    LIBATAPP_MACRO_API const util::config::ini_loader &app::get_configure() const { return cfg_loader_; }

    LIBATAPP_MACRO_API bool app::add_log_sink_maker(const std::string &name, log_sink_maker::log_reg_t fn) {
        if (log_reg_.end() != log_reg_.find(name)) {
            return false;
        }

        log_reg_[name] = fn;
        return true;
    }

    LIBATAPP_MACRO_API void app::set_evt_on_recv_msg(callback_fn_on_msg_t fn) { evt_on_recv_msg_ = fn; }
    LIBATAPP_MACRO_API void app::set_evt_on_forward_response(callback_fn_on_forward_response_t fn) { evt_on_forward_response_ = fn; }
    LIBATAPP_MACRO_API void app::set_evt_on_app_connected(callback_fn_on_connected_t fn) { evt_on_app_connected_ = fn; }
    LIBATAPP_MACRO_API void app::set_evt_on_app_disconnected(callback_fn_on_disconnected_t fn) { evt_on_app_disconnected_ = fn; }
    LIBATAPP_MACRO_API void app::set_evt_on_all_module_inited(callback_fn_on_all_module_inited_t fn) { evt_on_all_module_inited_ = fn; }

    LIBATAPP_MACRO_API const app::callback_fn_on_msg_t &app::get_evt_on_recv_msg() const { return evt_on_recv_msg_; }
    LIBATAPP_MACRO_API const app::callback_fn_on_forward_response_t &app::get_evt_on_send_fail() const { return evt_on_forward_response_; }
    LIBATAPP_MACRO_API const app::callback_fn_on_connected_t &app::get_evt_on_app_connected() const { return evt_on_app_connected_; }
    LIBATAPP_MACRO_API const app::callback_fn_on_disconnected_t &app::get_evt_on_app_disconnected() const { return evt_on_app_disconnected_; }
    LIBATAPP_MACRO_API const app::callback_fn_on_all_module_inited_t &app::get_evt_on_all_module_inited() const { return evt_on_all_module_inited_; }

    void app::ev_stop_timeout(uv_timer_t *handle) {
        assert(handle && handle->data);

        if (NULL != handle && NULL != handle->data) {
            app *self = reinterpret_cast<app *>(handle->data);
            self->set_flag(flag_t::TIMEOUT, true);
        }

        if (NULL != handle) {
            uv_stop(handle->loop);
        }
    }

    bool app::set_flag(flag_t::type f, bool v) {
        if (f < 0 || f >= flag_t::FLAG_MAX) {
            return false;
        }

        bool ret = flags_.test(f);
        flags_.set(f, v);
        return ret;
    }

    LIBATAPP_MACRO_API bool app::check_flag(flag_t::type f) const {
        if (f < 0 || f >= flag_t::FLAG_MAX) {
            return false;
        }

        return flags_.test(f);
    }

    int app::apply_configure() {
        // id and id mask
        if (conf_.id_mask.empty()) {
            std::string id_mask_str;
            cfg_loader_.dump_to("atapp.id_mask", id_mask_str);
            split_ids_by_string(id_mask_str.c_str(), conf_.id_mask);
        }

        if (!conf_.id_cmd.empty()) {
            conf_.id = convert_app_id_by_string(conf_.id_cmd.c_str());
        }

        if (0 == conf_.id) {
            std::string id_cfg;
            cfg_loader_.dump_to("atapp.id", id_cfg);
            conf_.id = convert_app_id_by_string(id_cfg.c_str());
        }

        cfg_loader_.dump_to("atapp.name", conf_.name, true);
        cfg_loader_.dump_to("atapp.type_id", conf_.type_id);
        cfg_loader_.dump_to("atapp.type_name", conf_.type_name);
        if (conf_.name.empty()) {
            std::stringstream ss;
            ss << conf_.type_name << "-0x" << std::ios::hex << conf_.id;
            conf_.name = ss.str();
        }
        {
            uint64_t hash_out[2];
            ::util::hash::murmur_hash3_x64_128(conf_.name.c_str(), static_cast<int>(conf_.name.size()), 0x01000193U, hash_out);
            char hash_code_str[40] = {0};
            UTIL_STRFUNC_SNPRINTF(hash_code_str, sizeof(hash_code_str), "%016llX%016llX", static_cast<unsigned long long>(hash_out[0]),
                                  static_cast<unsigned long long>(hash_out[1]));
            conf_.hash_code = &hash_code_str[0];
        }

        cfg_loader_.dump_to("atapp.remove_pidfile_after_exit", conf_.remove_pidfile_after_exit);

        // hostname
        {
            std::string hostname;
            cfg_loader_.dump_to("atapp.hostname", hostname);
            if (!hostname.empty()) {
                atbus::node::set_hostname(hostname);
            }
        }

        conf_.bus_listen.clear();
        cfg_loader_.dump_to("atapp.bus.listen", conf_.bus_listen);

        // conf_.stop_timeout = 30000; // use last available value
        {
            util::config::duration_value duration;
            cfg_loader_.dump_to("atapp.timer.stop_timeout", duration);
            conf_.stop_timeout = duration.sec;
        }

        // conf_.tick_interval = 32; // use last available value
        {
            util::config::duration_value duration;
            cfg_loader_.dump_to("atapp.timer.tick_interval", duration);
            conf_.tick_interval = duration.sec;
        }

        // atbus configure
        atbus::node::default_conf(&conf_.bus_conf);

        {
            std::vector<std::string> subnets_conf;
            cfg_loader_.dump_to("atapp.bus.subnets", subnets_conf);
            conf_.bus_conf.subnets.reserve(subnets_conf.size());
            for (size_t i = 0; i < subnets_conf.size(); ++ i) {
                std::string::size_type sep_pos = subnets_conf[i].find('/');
                if (std::string::npos == sep_pos) {
                    conf_.bus_conf.subnets.push_back(atbus::endpoint_subnet_conf(
                        0, util::string::to_int<uint32_t>(subnets_conf[i].c_str())
                    ));
                } else {
                    conf_.bus_conf.subnets.push_back(atbus::endpoint_subnet_conf(
                        convert_app_id_by_string(subnets_conf[i].c_str()), 
                        util::string::to_int<uint32_t>(subnets_conf[i].c_str() + sep_pos + 1)
                    ));
                }
            }
        }
        cfg_loader_.dump_to("atapp.bus.proxy", conf_.bus_conf.parent_address);
        cfg_loader_.dump_to("atapp.bus.loop_times", conf_.bus_conf.loop_times);
        cfg_loader_.dump_to("atapp.bus.ttl", conf_.bus_conf.ttl);
        cfg_loader_.dump_to("atapp.bus.backlog", conf_.bus_conf.backlog);
        cfg_loader_.dump_to("atapp.bus.access_token_max_number", conf_.bus_conf.access_token_max_number);
        {
            std::vector<std::string> access_tokens;
            cfg_loader_.dump_to("atapp.bus.access_tokens", access_tokens);
            conf_.bus_conf.access_tokens.reserve(access_tokens.size());
            for (size_t i = 0; i < access_tokens.size(); ++ i) {
                conf_.bus_conf.access_tokens.push_back(std::vector<unsigned char>());
                conf_.bus_conf.access_tokens.back().assign(
                    reinterpret_cast<const unsigned char*>(access_tokens[i].data()), 
                    reinterpret_cast<const unsigned char*>(access_tokens[i].data()) + access_tokens[i].size()
                );
            }
        }

        {
            util::config::duration_value duration;
            cfg_loader_.dump_to("atapp.bus.first_idle_timeout", duration);
            conf_.bus_conf.first_idle_timeout = duration.sec;
        }
        {
            util::config::duration_value duration;
            cfg_loader_.dump_to("atapp.bus.ping_interval", duration);
            conf_.bus_conf.ping_interval = duration.sec;
        }
        {
            util::config::duration_value duration;
            cfg_loader_.dump_to("atapp.bus.retry_interval", duration);
            conf_.bus_conf.retry_interval = duration.sec;
        }
        
        cfg_loader_.dump_to("atapp.bus.fault_tolerant", conf_.bus_conf.fault_tolerant);
        cfg_loader_.dump_to("atapp.bus.msg_size", conf_.bus_conf.msg_size);
        cfg_loader_.dump_to("atapp.bus.recv_buffer_size", conf_.bus_conf.recv_buffer_size);
        cfg_loader_.dump_to("atapp.bus.send_buffer_size", conf_.bus_conf.send_buffer_size);
        cfg_loader_.dump_to("atapp.bus.send_buffer_number", conf_.bus_conf.send_buffer_number);

        return 0;
    } // namespace atapp

    void app::run_ev_loop(int run_mode) {
        util::time::time_utility::update();

        if (bus_node_) {
            // step X. loop uv_run util stop flag is set
            assert(bus_node_->get_evloop());
            if (NULL != bus_node_->get_evloop()) {
                flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
                uv_run(bus_node_->get_evloop(), static_cast<uv_run_mode>(run_mode));
            }

            if (check_flag(flag_t::RESET_TIMER)) {
                setup_timer();
            }

            if (check_flag(flag_t::STOPING)) {
                set_flag(flag_t::STOPPED, true);

                if (check_flag(flag_t::TIMEOUT)) {
                    // step X. notify all modules timeout
                    owent_foreach(module_ptr_t & mod, modules_) {
                        if (mod->is_enabled()) {
                            WLOGERROR("try to stop module %s but timeout", mod->name());
                            mod->timeout();
                            mod->disable();
                        }
                    }
                } else {
                    // step X. notify all modules to finish and wait for all modules stop
                    owent_foreach(module_ptr_t & mod, modules_) {
                        if (mod->is_enabled()) {
                            int res = mod->stop();
                            if (0 == res) {
                                mod->disable();
                            } else if (res < 0) {
                                mod->disable();
                                WLOGERROR("try to stop module %s but failed and return %d", mod->name(), res);
                            } else {
                                // any module stop running will make app wait
                                set_flag(flag_t::STOPPED, false);
                            }
                        }
                    }

                    // step X. if stop is blocked and timeout not triggered, setup stop timeout and waiting for all modules finished
                    if (false == tick_timer_.timeout_timer.is_activited) {
                        uv_timer_init(bus_node_->get_evloop(), &tick_timer_.timeout_timer.timer);
                        tick_timer_.timeout_timer.timer.data = this;

                        int res = uv_timer_start(&tick_timer_.timeout_timer.timer, ev_stop_timeout, conf_.stop_timeout, 0);
                        if (0 == res) {
                            tick_timer_.timeout_timer.is_activited = true;
                        } else {
                            WLOGERROR("setup stop timeout failed, res: %d", res);
                            set_flag(flag_t::TIMEOUT, false);
                        }
                    }
                }
            }

            // if atbus is at shutdown state, loop event dispatcher using next while iterator
        }
    }

    int app::run_inner(int run_mode) {
        if (false == check_flag(flag_t::INITIALIZED)) {
            return EN_ATAPP_ERR_NOT_INITED;
        }

        last_proc_event_count_ = 0;
        if (check_flag(flag_t::IN_CALLBACK)) {
            return 0;
        }

        if (mode_t::START != mode_) {
            return 0;
        }

        // TODO if atbus is reset, init it again

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

            // cleanup pid file
            cleanup_pidfile();

            set_flag(flag_t::INITIALIZED, false);
            set_flag(flag_t::RUNNING, false);
        }

        if (last_proc_event_count_ > 0) {
            return 1;
        }

        return 0;
    }

    // graceful Exits
    void app::_app_setup_signal_term(int /*signo*/) {
        if (NULL != app::last_instance_) {
            app::last_instance_->stop();
        }
    }

    int app::setup_signal() {
        // block signals
        app::last_instance_ = this;
        signal(SIGTERM, _app_setup_signal_term);
        signal(SIGINT, SIG_IGN);

#ifndef WIN32
        signal(SIGSTOP, _app_setup_signal_term);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);  // lost parent process
        signal(SIGPIPE, SIG_IGN); // close stdin, stdout or stderr
        signal(SIGTSTP, SIG_IGN); // close tty
        signal(SIGTTIN, SIG_IGN); // tty input
        signal(SIGTTOU, SIG_IGN); // tty output
#endif

        return 0;
    }

    int app::setup_log() {
        util::cli::shell_stream ss(std::cerr);

        // register inner log module
        if (log_reg_.find(log_sink_maker::get_file_sink_name()) == log_reg_.end()) {
            log_reg_[log_sink_maker::get_file_sink_name()] = log_sink_maker::get_file_sink_reg();
        }

        if (log_reg_.find(log_sink_maker::get_stdout_sink_name()) == log_reg_.end()) {
            log_reg_[log_sink_maker::get_stdout_sink_name()] = log_sink_maker::get_stdout_sink_reg();
        }

        if (log_reg_.find(log_sink_maker::get_stderr_sink_name()) == log_reg_.end()) {
            log_reg_[log_sink_maker::get_stderr_sink_name()] = log_sink_maker::get_stderr_sink_reg();
        }

        if (false == is_running()) {
            // if inited, let all modules setup custom logger
            owent_foreach(module_ptr_t & mod, modules_) {
                if (mod && mod->is_enabled()) {
                    int res = mod->setup_log();
                    if (0 != res) {
                        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "setup log for module " << mod->name() << " failed, result: " << res << "."
                             << std::endl;
                        return res;
                    }
                }
            }
        }

        // load configure
        uint32_t log_cat_number = LOG_WRAPPER_CATEGORIZE_SIZE;
        cfg_loader_.dump_to("atapp.log.cat.number", log_cat_number);
        if (log_cat_number > LOG_WRAPPER_CATEGORIZE_SIZE) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "log categorize should not be greater than " << LOG_WRAPPER_CATEGORIZE_SIZE
                 << ". you can define LOG_WRAPPER_CATEGORIZE_SIZE to a greater number and rebuild atapp." << std::endl;
            log_cat_number = LOG_WRAPPER_CATEGORIZE_SIZE;
        }
        int log_level_id = util::log::log_wrapper::level_t::LOG_LW_INFO;
        std::string log_level_name;
        cfg_loader_.dump_to("atapp.log.level", log_level_name);
        log_level_id = util::log::log_formatter::get_level_by_name(log_level_name.c_str());

        char log_path[256] = {0};

        for (uint32_t i = 0; i < log_cat_number; ++i) {
            std::string log_name, log_prefix, log_stacktrace_min, log_stacktrace_max;
            UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.cat.%u.name", i);
            cfg_loader_.dump_to(log_path, log_name);

            if (log_name.empty()) {
                continue;
            }

            UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.cat.%u.prefix", i);
            cfg_loader_.dump_to(log_path, log_prefix);

            UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.cat.%u.stacktrace.min", i);
            cfg_loader_.dump_to(log_path, log_stacktrace_min);

            UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.cat.%u.stacktrace.max", i);
            cfg_loader_.dump_to(log_path, log_stacktrace_max);

            // init and set prefix
            if (0 != (WLOG_INIT(i, WLOG_LEVELID(log_level_id)))) {
                ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "log init " << log_name << "(" << i << ") failed, skipped." << std::endl;
                continue;
            }
            if (!log_prefix.empty()) {
                WLOG_GETCAT(i)->set_prefix_format(log_prefix);
            }

            // load stacktrace configure
            if (!log_stacktrace_min.empty() || !log_stacktrace_max.empty()) {
                util::log::log_formatter::level_t::type stacktrace_level_min = util::log::log_formatter::level_t::LOG_LW_DISABLED;
                util::log::log_formatter::level_t::type stacktrace_level_max = util::log::log_formatter::level_t::LOG_LW_DISABLED;
                if (!log_stacktrace_min.empty()) {
                    stacktrace_level_min = util::log::log_formatter::get_level_by_name(log_stacktrace_min.c_str());
                }

                if (!log_stacktrace_max.empty()) {
                    stacktrace_level_max = util::log::log_formatter::get_level_by_name(log_stacktrace_max.c_str());
                }

                WLOG_GETCAT(i)->set_stacktrace_level(stacktrace_level_max, stacktrace_level_min);
            }

            // For now, only log level can be reload
            size_t old_sink_number = WLOG_GETCAT(i)->sink_size();
            size_t new_sink_number = 0;

            // register log handles
            for (uint32_t j = 0;; ++j) {
                std::string sink_type;
                UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.%s.%u.type", log_name.c_str(), j);
                cfg_loader_.dump_to(log_path, sink_type);

                if (sink_type.empty()) {
                    break;
                }

                // already read log cat name, sink type name
                int log_handle_min = util::log::log_wrapper::level_t::LOG_LW_FATAL, log_handle_max = util::log::log_wrapper::level_t::LOG_LW_DEBUG;
                UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.%s.%u", log_name.c_str(), j);
                util::config::ini_value &cfg_set = cfg_loader_.get_node(log_path);

                UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.%s.%u.level.min", log_name.c_str(), j);
                log_level_name.clear();
                cfg_loader_.dump_to(log_path, log_level_name);
                log_handle_min = util::log::log_formatter::get_level_by_name(log_level_name.c_str());

                UTIL_STRFUNC_SNPRINTF(log_path, sizeof(log_path), "atapp.log.%s.%u.level.max", log_name.c_str(), j);
                log_level_name.clear();
                cfg_loader_.dump_to(log_path, log_level_name);
                log_handle_max = util::log::log_formatter::get_level_by_name(log_level_name.c_str());

                // register log sink
                if (new_sink_number >= old_sink_number) {
                    std::map<std::string, log_sink_maker::log_reg_t>::iterator iter = log_reg_.find(sink_type);
                    if (iter != log_reg_.end()) {
                        util::log::log_wrapper::log_handler_t log_handler = iter->second(log_name, *WLOG_GETCAT(i), j, cfg_set);
                        WLOG_GETCAT(i)->add_sink(log_handler, static_cast<util::log::log_wrapper::level_t::type>(log_handle_min),
                                                 static_cast<util::log::log_wrapper::level_t::type>(log_handle_max));
                        ++new_sink_number;
                    } else {
                        ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "unavailable log type " << sink_type
                             << ", you can add log type register handle before init." << std::endl;
                    }
                } else {
                    WLOG_GETCAT(i)->set_sink(new_sink_number, static_cast<util::log::log_wrapper::level_t::type>(log_handle_min),
                                             static_cast<util::log::log_wrapper::level_t::type>(log_handle_max));
                    ++new_sink_number;
                }
            }

            while (WLOG_GETCAT(i)->sink_size() > new_sink_number) {
                WLOG_GETCAT(i)->pop_sink();
            }
        }

        return 0;
    }

    int app::setup_atbus() {
        int ret = 0, res = 0;
        if (bus_node_) {
            bus_node_->reset();
            bus_node_.reset();
        }

        std::shared_ptr<atbus::node> connection_node = atbus::node::create();
        if (!connection_node) {
            WLOGERROR("create bus node failed.");
            return EN_ATAPP_ERR_SETUP_ATBUS;
        }

        ret = connection_node->init(conf_.id, &conf_.bus_conf);
        if (ret < 0) {
            WLOGERROR("init bus node failed. ret: %d", ret);
            return EN_ATAPP_ERR_SETUP_ATBUS;
        }

        // setup all callbacks
        connection_node->set_on_recv_handle(std::bind(&app::bus_evt_callback_on_recv_msg, this, std::placeholders::_1, std::placeholders::_2,
                                                      std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6));

        connection_node->set_on_forward_response_handle(
            std::bind(&app::bus_evt_callback_on_forward_response, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));

        connection_node->set_on_error_handle(std::bind(&app::bus_evt_callback_on_error, this, std::placeholders::_1, std::placeholders::_2,
                                                       std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

        connection_node->set_on_register_handle(
            std::bind(&app::bus_evt_callback_on_reg, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));

        connection_node->set_on_shutdown_handle(std::bind(&app::bus_evt_callback_on_shutdown, this, std::placeholders::_1, std::placeholders::_2));

        connection_node->set_on_available_handle(std::bind(&app::bus_evt_callback_on_available, this, std::placeholders::_1, std::placeholders::_2));

        connection_node->set_on_invalid_connection_handle(
            std::bind(&app::bus_evt_callback_on_invalid_connection, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        connection_node->set_on_custom_cmd_handle(std::bind(&app::bus_evt_callback_on_custom_cmd, this, std::placeholders::_1, std::placeholders::_2,
                                                            std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6));
        connection_node->set_on_add_endpoint_handle(
            std::bind(&app::bus_evt_callback_on_add_endpoint, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        connection_node->set_on_remove_endpoint_handle(
            std::bind(&app::bus_evt_callback_on_remove_endpoint, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));


        // TODO if not in resume mode, destroy shm
        // if (false == conf_.resume_mode) {}

        // init listen
        for (size_t i = 0; i < conf_.bus_listen.size(); ++i) {
            res = connection_node->listen(conf_.bus_listen[i].c_str());
            if (res < 0) {
#ifdef _WIN32
                if (EN_ATBUS_ERR_SHM_GET_FAILED == res) {
                    WLOGERROR("Using global shared memory require SeCreateGlobalPrivilege, try to run as Administrator.\nWe will ignore %s this time.",
                              conf_.bus_listen[i].c_str());
                    util::cli::shell_stream ss(std::cerr);
                    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED
                         << "Using global shared memory require SeCreateGlobalPrivilege, try to run as Administrator." << std::endl
                         << "We will ignore " << conf_.bus_listen[i] << " this time." << std::endl;

                    // res = 0; // Value stored to 'res' is never read
                } else {
#endif
                    WLOGERROR("bus node listen %s failed. res: %d", conf_.bus_listen[i].c_str(), res);
                    if (EN_ATBUS_ERR_PIPE_ADDR_TOO_LONG == res) {
                        atbus::channel::channel_address_t address;
                        atbus::channel::make_address(conf_.bus_listen[i].c_str(), address);
                        std::string abs_path = util::file_system::get_abs_path(address.host.c_str());
                        WLOGERROR("listen pipe socket %s, but the length (%llu) exceed the limit %llu", abs_path.c_str(),
                                  static_cast<unsigned long long>(abs_path.size()),
                                  static_cast<unsigned long long>(atbus::channel::io_stream_get_max_unix_socket_length()));
                    }
                    ret = res;
#ifdef _WIN32
                }
#endif
            }
        }

        if (ret < 0) {
            WLOGERROR("bus node listen failed");
            return ret;
        }

        // start
        ret = connection_node->start();
        if (ret < 0) {
            WLOGERROR("bus node start failed, ret: %d", ret);
            return ret;
        }

        // if has father node, block and connect to father node
        if (atbus::node::state_t::CONNECTING_PARENT == connection_node->get_state() || atbus::node::state_t::LOST_PARENT == connection_node->get_state()) {
            // setup timeout and waiting for parent connected
            if (false == tick_timer_.timeout_timer.is_activited) {
                uv_timer_init(connection_node->get_evloop(), &tick_timer_.timeout_timer.timer);
                tick_timer_.timeout_timer.timer.data = this;

                res = uv_timer_start(&tick_timer_.timeout_timer.timer, ev_stop_timeout, conf_.stop_timeout, 0);
                if (0 == res) {
                    tick_timer_.timeout_timer.is_activited = true;
                } else {
                    WLOGERROR("setup stop timeout failed, res: %d", res);
                    set_flag(flag_t::TIMEOUT, false);
                }

                while (NULL == connection_node->get_parent_endpoint()) {
                    if (check_flag(flag_t::TIMEOUT)) {
                        WLOGERROR("connection to parent node %s timeout", conf_.bus_conf.parent_address.c_str());
                        ret = -1;
                        break;
                    }

                    {
                        flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
                        uv_run(connection_node->get_evloop(), UV_RUN_ONCE);
                    }
                }

                // if connected, do not trigger timeout
                close_timer(tick_timer_.timeout_timer);

                if (ret < 0) {
                    WLOGERROR("connect to parent node failed");
                    return ret;
                }
            }
        }

        bus_node_ = connection_node;

        return 0;
    }

    void app::close_timer(timer_info_t &t) {
        if (t.is_activited) {
            uv_timer_stop(&t.timer);
            uv_close(reinterpret_cast<uv_handle_t *>(&t.timer), NULL);
            t.is_activited = false;
        }
    }

    static void _app_tick_timer_handle(uv_timer_t *handle) {
        assert(handle && handle->data);

        if (NULL != handle && NULL != handle->data) {
            app *self = reinterpret_cast<app *>(handle->data);
            self->tick();
        }
    }

    int app::setup_timer() {
        set_flag(flag_t::RESET_TIMER, false);

        close_timer(tick_timer_.tick_timer);

        if (conf_.tick_interval < 1) {
            conf_.tick_interval = 1;
            WLOGWARNING("tick interval can not smaller than 1ms, we use 1ms now.");
        } else {
            WLOGINFO("setup tick interval to %llums.", static_cast<unsigned long long>(conf_.tick_interval));
        }

        uv_timer_init(bus_node_->get_evloop(), &tick_timer_.tick_timer.timer);
        tick_timer_.tick_timer.timer.data = this;

        int res = uv_timer_start(&tick_timer_.tick_timer.timer, _app_tick_timer_handle, conf_.tick_interval, conf_.tick_interval);
        if (0 == res) {
            tick_timer_.tick_timer.is_activited = true;
        } else {
            WLOGERROR("setup tick timer failed, res: %d", res);
            return EN_ATAPP_ERR_SETUP_TIMER;
        }

        return 0;
    }

    bool app::write_pidfile() {
        if (!conf_.pid_file.empty()) {
            std::fstream pid_file;
            pid_file.open(conf_.pid_file.c_str(), std::ios::out | std::ios::trunc);
            if (!pid_file.is_open()) {
                util::cli::shell_stream ss(std::cerr);
                ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "open and write pid file " << conf_.pid_file << " failed" << std::endl;
                WLOGERROR("open and write pid file %s failed", conf_.pid_file.c_str());
                // failed and skip running
                return false;
            } else {
                pid_file << atbus::node::get_pid();
                pid_file.close();
            }
        }

        return true;
    }

    bool app::cleanup_pidfile() {
        if (conf_.remove_pidfile_after_exit && !conf_.pid_file.empty()) {
            std::fstream pid_file;

            pid_file.open(conf_.pid_file.c_str(), std::ios::in);
            if (!pid_file.is_open()) {
                util::cli::shell_stream ss(std::cerr);
                ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "try to remove pid file " << conf_.pid_file << " failed" << std::endl;

                // failed and skip running
                return false;
            } else {
                int pid = 0;
                pid_file >> pid;
                pid_file.close();

                if (pid != atbus::node::get_pid()) {
                    util::cli::shell_stream ss(std::cerr);
                    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << "skip remove pid file " << conf_.pid_file << ". because it has pid " << pid
                         << ", but our pid is " << atbus::node::get_pid() << std::endl;

                    return false;
                } else {
                    return util::file_system::remove(conf_.pid_file.c_str());
                }
            }
        }

        return true;
    }

    void app::print_help() {
        util::cli::shell_stream shls(std::cout);

        shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD << "Usage: " << conf_.execute_path
               << " <options> <command> [command paraters...]" << std::endl;
        shls() << get_option_manager()->get_help_msg() << std::endl << std::endl;

        if (!(get_command_manager()->empty() && get_command_manager()->children_empty())) {
            shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD
                   << "Custom command help:" << std::endl;
            shls() << get_command_manager()->get_help_msg() << std::endl;
        }
    }

    LIBATAPP_MACRO_API app::custom_command_sender_t app::get_custom_command_sender(util::cli::callback_param params) {
        custom_command_sender_t ret;
        ret.self = NULL;
        ret.response = NULL;
        if (NULL != params.get_ext_param()) {
            ret = *reinterpret_cast<custom_command_sender_t *>(params.get_ext_param());
        }

        return ret;
    }

    LIBATAPP_MACRO_API bool app::add_custom_command_rsp(util::cli::callback_param params, const std::string &rsp_text) {
        custom_command_sender_t sender = get_custom_command_sender(params);
        if (NULL == sender.response) {
            return false;
        }

        sender.response->push_back(rsp_text);
        return true;
    }

    LIBATAPP_MACRO_API void app::split_ids_by_string(const char *in, std::vector<app_id_t> &out) {
        if (NULL == in) {
            return;
        }

        out.reserve(8);

        while (NULL != in && *in) {
            // skip spaces
            if (' ' == *in || '\t' == *in || '\r' == *in || '\n' == *in) {
                ++in;
                continue;
            }

            out.push_back(::util::string::to_int<app_id_t>(in));

            for (; NULL != in && *in && '.' != *in; ++in)
                ;
            // skip dot and ready to next segment
            if (NULL != in && *in && '.' == *in) {
                ++in;
            }
        }
    }

    LIBATAPP_MACRO_API app::app_id_t app::convert_app_id_by_string(const char *id_in, const std::vector<app_id_t> &mask_in) {
        if (NULL == id_in || 0 == *id_in) {
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
        if (NULL == id_in || 0 == *id_in) {
            return 0;
        }

        std::vector<app_id_t> mask;
        split_ids_by_string(mask_in, mask);
        return convert_app_id_by_string(id_in, mask);
    }

    int app::prog_option_handler_help(util::cli::callback_param /*params*/, util::cli::cmd_option *opt_mgr, util::cli::cmd_option_ci *cmd_mgr) {
        assert(opt_mgr);
        mode_ = mode_t::INFO;
        util::cli::shell_stream shls(std::cout);

        shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD << "Usage: " << conf_.execute_path
               << " <options> <command> [command paraters...]" << std::endl;
        shls() << opt_mgr->get_help_msg() << std::endl << std::endl;

        shls() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << util::cli::shell_font_style::SHELL_FONT_SPEC_BOLD
               << "Custom command help:" << std::endl;
        shls() << cmd_mgr->get_help_msg() << std::endl;
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
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "-c, --conf, --config require 1 parameter" << std::endl;
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

    int app::prog_option_handler_resume_mode(util::cli::callback_param /*params*/) {
        conf_.resume_mode = true;
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
        return 0;
    }

    int app::prog_option_handler_reload(util::cli::callback_param /*params*/) {
        mode_ = mode_t::RELOAD;
        last_command_.clear();
        last_command_.push_back("reload");
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
        opt_mgr->bind_cmd("-id", &app::prog_option_handler_set_id, this)->set_help_msg("-id <bus id>                           set app bus id.");
        // set app bus id
        opt_mgr->bind_cmd("-id-mask", &app::prog_option_handler_set_id_mask, this)
            ->set_help_msg("-id-mask <bit number of bus id mask>   set app bus id mask(example: 8.8.8.8, and then -id 1.2.3.4 is just like -id 0x01020304).");

        // set configure file path
        opt_mgr->bind_cmd("-c, --conf, --config", &app::prog_option_handler_set_conf_file, this)
            ->set_help_msg("-c, --conf, --config <file path>       set configure file path.");

        // set app pid file
        opt_mgr->bind_cmd("-p, --pid", &app::prog_option_handler_set_pid, this)->set_help_msg("-p, --pid <pid file>                   set where to store pid.");

        // set configure file path
        opt_mgr->bind_cmd("-r, --resume", &app::prog_option_handler_resume_mode, this)
            ->set_help_msg("-r, --resume                           try to resume when start.");

        // start server
        opt_mgr->bind_cmd("start", &app::prog_option_handler_start, this)->set_help_msg("start                                  start mode.");

        // stop server
        opt_mgr->bind_cmd("stop", &app::prog_option_handler_stop, this)->set_help_msg("stop                                   send stop command to server.");

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
        UTIL_STRFUNC_SNPRINTF(msg, sizeof(msg), "app node 0x%llx run stop command success", static_cast<unsigned long long>(get_id()));
        WLOGINFO("%s", msg);
        add_custom_command_rsp(params, msg);
        return stop();
    }

    int app::command_handler_reload(util::cli::callback_param params) {
        char msg[256] = {0};
        UTIL_STRFUNC_SNPRINTF(msg, sizeof(msg), "app node 0x%llx run reload command success", static_cast<unsigned long long>(get_id()));
        WLOGINFO("%s", msg);
        add_custom_command_rsp(params, msg);
        return reload();
    }

    int app::command_handler_invalid(util::cli::callback_param params) {
        char msg[256] = {0};
        UTIL_STRFUNC_SNPRINTF(msg, sizeof(msg), "receive invalid command %s", params.get("@Cmd")->to_string());
        WLOGERROR("%s", msg);
        add_custom_command_rsp(params, msg);
        return 0;
    }

    int app::bus_evt_callback_on_recv_msg(const atbus::node &, const atbus::endpoint *, const atbus::connection *, const msg_t &msg, const void *buffer,
                                          size_t len) {
        // call recv callback
        if (evt_on_recv_msg_) {
            return evt_on_recv_msg_(std::ref(*this), std::cref(msg), buffer, len);
        }

        ++last_proc_event_count_;
        return 0;
    }

    int app::bus_evt_callback_on_forward_response(const atbus::node &, const atbus::endpoint *, const atbus::connection *, const atbus::protocol::msg *m) {
        ++last_proc_event_count_;

        // call failed callback if it's message transfer
        if (NULL == m) {
            WLOGERROR("app 0x%llx receive a send failure without message", static_cast<unsigned long long>(get_id()));
            return EN_ATAPP_ERR_SEND_FAILED;
        }

        WLOGERROR("app 0x%llx receive a send failure from 0x%llx, message cmd: %d, type: %d, ret: %d, sequence: %llu",
                  static_cast<unsigned long long>(get_id()), static_cast<unsigned long long>(m->head().src_bus_id()), 
                  atbus::msg_handler::get_body_name(m->msg_body_case()), m->head().type(),
                  m->head().ret(), static_cast<unsigned long long>(m->head().sequence()));

        if (evt_on_forward_response_) {
            const atbus::protocol::forward_data* fwd_data = NULL;
            if (atbus::protocol::msg::kDataTransformReq == m->msg_body_case()) {
                fwd_data = &m->data_transform_req();
            } else if (atbus::protocol::msg::kDataTransformRsp == m->msg_body_case()) {
                fwd_data = &m->data_transform_rsp();
            }
            if (NULL != fwd_data) {
                app_id_t origin_from = fwd_data->to();
                app_id_t origin_to = fwd_data->from();
                return evt_on_forward_response_(std::ref(*this), origin_from, origin_to, std::cref(*m));
            }
        }

        return 0;
    }

    int app::bus_evt_callback_on_error(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn, int status, int errcode) {

        // meet eof or reset by peer is not a error
        if (UV_EOF == errcode || UV_ECONNRESET == errcode) {
            const char *msg = UV_EOF == errcode ? "got EOF" : "reset by peer";
            if (NULL != conn) {
                if (NULL != ep) {
                    WLOGINFO("bus node 0x%llx endpoint 0x%llx connection %s closed: %s", static_cast<unsigned long long>(n.get_id()),
                             static_cast<unsigned long long>(ep->get_id()), conn->get_address().address.c_str(), msg);
                } else {
                    WLOGINFO("bus node 0x%llx connection %s closed: %s", static_cast<unsigned long long>(n.get_id()), conn->get_address().address.c_str(), msg);
                }

            } else {
                if (NULL != ep) {
                    WLOGINFO("bus node 0x%llx endpoint 0x%llx closed: %s", static_cast<unsigned long long>(n.get_id()),
                             static_cast<unsigned long long>(ep->get_id()), msg);
                } else {
                    WLOGINFO("bus node 0x%llx closed: %s", static_cast<unsigned long long>(n.get_id()), msg);
                }
            }
            return 0;
        }

        if (NULL != conn) {
            if (NULL != ep) {
                WLOGERROR("bus node 0x%llx endpoint 0x%llx connection %s error, status: %d, error code: %d", static_cast<unsigned long long>(n.get_id()),
                          static_cast<unsigned long long>(ep->get_id()), conn->get_address().address.c_str(), status, errcode);
            } else {
                WLOGERROR("bus node 0x%llx connection %s error, status: %d, error code: %d", static_cast<unsigned long long>(n.get_id()),
                          conn->get_address().address.c_str(), status, errcode);
            }

        } else {
            if (NULL != ep) {
                WLOGERROR("bus node 0x%llx endpoint 0x%llx error, status: %d, error code: %d", static_cast<unsigned long long>(n.get_id()),
                          static_cast<unsigned long long>(ep->get_id()), status, errcode);
            } else {
                WLOGERROR("bus node 0x%llx error, status: %d, error code: %d", static_cast<unsigned long long>(n.get_id()), status, errcode);
            }
        }

        return 0;
    }

    int app::bus_evt_callback_on_reg(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn, int res) {
        ++last_proc_event_count_;

        if (NULL != conn) {
            if (NULL != ep) {
                WLOGINFO("bus node 0x%llx endpoint 0x%llx connection %s registered, res: %d", static_cast<unsigned long long>(n.get_id()),
                         static_cast<unsigned long long>(ep->get_id()), conn->get_address().address.c_str(), res);
            } else {
                WLOGINFO("bus node 0x%llx connection %s registered, res: %d", static_cast<unsigned long long>(n.get_id()), conn->get_address().address.c_str(),
                         res);
            }

        } else {
            if (NULL != ep) {
                WLOGINFO("bus node 0x%llx endpoint 0x%llx registered, res: %d", static_cast<unsigned long long>(n.get_id()),
                         static_cast<unsigned long long>(ep->get_id()), res);
            } else {
                WLOGINFO("bus node 0x%llx registered, res: %d", static_cast<unsigned long long>(n.get_id()), res);
            }
        }

        return 0;
    }

    int app::bus_evt_callback_on_shutdown(const atbus::node &n, int reason) {
        WLOGINFO("bus node 0x%llx shutdown, reason: %d", static_cast<unsigned long long>(n.get_id()), reason);
        return stop();
    }

    int app::bus_evt_callback_on_available(const atbus::node &n, int res) {
        WLOGINFO("bus node 0x%llx initialze done, res: %d", static_cast<unsigned long long>(n.get_id()), res);
        return res;
    }

    int app::bus_evt_callback_on_invalid_connection(const atbus::node &n, const atbus::connection *conn, int res) {
        ++last_proc_event_count_;

        if (NULL == conn) {
            WLOGERROR("bus node 0x%llx recv a invalid NULL connection , res: %d", static_cast<unsigned long long>(n.get_id()), res);
        } else {
            // already disconncted finished.
            if (atbus::connection::state_t::DISCONNECTED != conn->get_status()) {
                WLOGERROR("bus node 0x%llx make connection to %s done, res: %d", static_cast<unsigned long long>(n.get_id()),
                          conn->get_address().address.c_str(), res);
            }
        }
        return 0;
    }

    int app::bus_evt_callback_on_custom_cmd(const atbus::node &, const atbus::endpoint *, const atbus::connection *, app_id_t /*src_id*/,
                                            const std::vector<std::pair<const void *, size_t> > &args, std::list<std::string> &rsp) {
        ++last_proc_event_count_;
        if (args.empty()) {
            return 0;
        }

        std::vector<std::string> args_str;
        args_str.resize(args.size());

        for (size_t i = 0; i < args_str.size(); ++i) {
            args_str[i].assign(reinterpret_cast<const char *>(args[i].first), args[i].second);
        }

        util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();
        custom_command_sender_t sender;
        sender.self = this;
        sender.response = &rsp;
        cmd_mgr->start(args_str, true, &sender);
        return 0;
    }

    int app::bus_evt_callback_on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
        ++last_proc_event_count_;

        if (NULL == ep) {
            WLOGERROR("bus node 0x%llx make connection to NULL, res: %d", static_cast<unsigned long long>(n.get_id()), res);
        } else {
            WLOGINFO("bus node 0x%llx make connection to 0x%llx done, res: %d", static_cast<unsigned long long>(n.get_id()),
                     static_cast<unsigned long long>(ep->get_id()), res);

            if (evt_on_app_connected_) {
                evt_on_app_connected_(std::ref(*this), std::ref(*ep), res);
            }
        }
        return 0;
    }

    int app::bus_evt_callback_on_remove_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
        ++last_proc_event_count_;

        if (NULL == ep) {
            WLOGERROR("bus node 0x%llx release connection to NULL, res: %d", static_cast<unsigned long long>(n.get_id()), res);
        } else {
            WLOGINFO("bus node 0x%llx release connection to 0x%llx done, res: %d", static_cast<unsigned long long>(n.get_id()),
                     static_cast<unsigned long long>(ep->get_id()), res);

            if (evt_on_app_disconnected_) {
                evt_on_app_disconnected_(std::ref(*this), std::ref(*ep), res);
            }
        }
        return 0;
    }

    static size_t __g_atapp_custom_cmd_rsp_recv_times = 0;
    int app::bus_evt_callback_on_custom_rsp(const atbus::node &, const atbus::endpoint *, const atbus::connection *, app_id_t src_id,
                                            const std::vector<std::pair<const void *, size_t> > &args, uint64_t /*seq*/) {
        ++last_proc_event_count_;
        ++__g_atapp_custom_cmd_rsp_recv_times;
        if (args.empty()) {
            return 0;
        }

        util::cli::shell_stream ss(std::cout);
        char bus_addr[64] = {0};
        UTIL_STRFUNC_SNPRINTF(bus_addr, sizeof(bus_addr), "0x%llx", static_cast<unsigned long long>(src_id));
        for (size_t i = 0; i < args.size(); ++i) {
            std::string text(static_cast<const char *>(args[i].first), args[i].second);
            ss() << "Custom Command: (" << bus_addr << "): " << text << std::endl;
        }

        return 0;
    }

    void app::setup_command() {
        util::cli::cmd_option_ci::ptr_type cmd_mgr = get_command_manager();
        // dump all connected nodes to default log collector
        // cmd_mgr->bind_cmd("dump");
        // dump all nodes to default log collector
        // cmd_mgr->bind_cmd("dump");
        // dump state

        // start server
        cmd_mgr->bind_cmd("start", &app::app::command_handler_start, this);
        // stop server
        cmd_mgr->bind_cmd("stop", &app::app::command_handler_stop, this);
        // reload all configures
        cmd_mgr->bind_cmd("reload", &app::app::command_handler_reload, this);

        // invalid command
        cmd_mgr->bind_cmd("@OnError", &app::command_handler_invalid, this);
    }

    int app::send_last_command(uv_loop_t *ev_loop) {
        util::cli::shell_stream ss(std::cerr);

        if (last_command_.empty()) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "command is empty." << std::endl;
            return EN_ATAPP_ERR_COMMAND_IS_NULL;
        }

        // step 1. using the fastest way to connect to server
        int use_level = 0;
        bool is_sync_channel = false;
        atbus::channel::channel_address_t use_addr;

        for (size_t i = 0; i < conf_.bus_listen.size(); ++i) {
            atbus::channel::channel_address_t parsed_addr;
            make_address(conf_.bus_listen[i].c_str(), parsed_addr);
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
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "there is no available listener address to send command." << std::endl;
            return EN_ATAPP_ERR_NO_AVAILABLE_ADDRESS;
        }

        if (!bus_node_) {
            bus_node_ = atbus::node::create();
        }

        // command mode , must no concurrence
        if (!bus_node_) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "create bus node failed" << std::endl;
            return EN_ATAPP_ERR_SETUP_ATBUS;
        }

        // no need to connect to parent node
        conf_.bus_conf.parent_address.clear();

        // using 0 for command sender
        int ret = bus_node_->init(0, &conf_.bus_conf);
        if (ret < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "init bus node failed. ret: " << ret << std::endl;
            return ret;
        }

        ret = bus_node_->start();
        if (ret < 0) {
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "start bus node failed. ret: " << ret << std::endl;
            return ret;
        }

        // step 2. connect failed return error code
        atbus::endpoint *ep = NULL;
        if (is_sync_channel) {
            // preallocate endpoint when using shared memory channel, because this channel can not be connected without endpoint
            std::vector<atbus::endpoint_subnet_conf> subnets;
            atbus::endpoint::ptr_t new_ep =
                atbus::endpoint::create(bus_node_.get(), conf_.id, subnets, bus_node_->get_pid(), bus_node_->get_hostname());
            ret = bus_node_->add_endpoint(new_ep);
            if (ret < 0) {
                ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "connect to " << use_addr.address << " failed. ret: " << ret << std::endl;
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
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "connect to " << use_addr.address << " failed. ret: " << ret << std::endl;
            return ret;
        }

        // step 3. setup timeout timer
        if (false == tick_timer_.timeout_timer.is_activited) {
            uv_timer_init(ev_loop, &tick_timer_.timeout_timer.timer);
            tick_timer_.timeout_timer.timer.data = this;

            int res = uv_timer_start(&tick_timer_.timeout_timer.timer, ev_stop_timeout, conf_.stop_timeout, 0);
            if (0 == res) {
                tick_timer_.timeout_timer.is_activited = true;
            } else {
                ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "setup timeout timer failed, res: " << res << std::endl;
                set_flag(flag_t::TIMEOUT, false);
            }
        }

        // step 4. waiting for connect success
        while (NULL == ep) {
            flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
            uv_run(ev_loop, UV_RUN_ONCE);

            if (check_flag(flag_t::TIMEOUT)) {
                break;
            }
            ep = bus_node_->get_endpoint(conf_.id);
        }

        if (NULL == ep) {
            close_timer(tick_timer_.timeout_timer);
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "connect to " << use_addr.address << " timeout." << std::endl;
            return EN_ATAPP_ERR_CONNECT_ATAPP_FAILED;
        }

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

        bus_node_->set_on_custom_rsp_handle(std::bind(&app::bus_evt_callback_on_custom_rsp, this, std::placeholders::_1, std::placeholders::_2,
                                                      std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6));

        ret = bus_node_->send_custom_cmd(ep->get_id(), &arr_buff[0], &arr_size[0], last_command_.size());
        if (ret < 0) {
            close_timer(tick_timer_.timeout_timer);
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "send command failed. ret: " << ret << std::endl;
            return ret;
        }

        // step 6. waiting for send done(for shm, no need to wait, for io_stream fd, waiting write callback)
        if (!is_sync_channel) {
            do {
                if (__g_atapp_custom_cmd_rsp_recv_times) {
                    break;
                }

                flag_guard_t in_callback_guard(*this, flag_t::IN_CALLBACK);
                uv_run(ev_loop, UV_RUN_ONCE);
                if (check_flag(flag_t::TIMEOUT)) {
                    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << "send command or receive response timeout" << std::endl;
                    ret = -1;
                    break;
                }
            } while (true);
        }

        close_timer(tick_timer_.timeout_timer);

        if (bus_node_) {
            bus_node_->reset();
            bus_node_.reset();
        }
        return ret;
    }
} // namespace atapp
