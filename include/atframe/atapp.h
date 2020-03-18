/**
 * atapp.h
 *
 *  Created on: 2016年04月23日
 *      Author: owent
 */
#ifndef LIBATAPP_ATAPP_H
#define LIBATAPP_ATAPP_H

#pragma once

#include <map>
#include <string>
#include <vector>

#include "std/functional.h"

#include <bitset>

#include "uv.h"

#include "cli/cmd_option.h"
#include "time/time_utility.h"

#include "atapp_conf.h"
#include "atapp_log_sink_maker.h"
#include "atapp_module_impl.h"


namespace atbus {
    namespace protocol {
        struct msg;
    }
    class node;
    class endpoint;
    class connection;
} // namespace atbus

namespace atapp {
    class app {
    public:
        typedef LIBATAPP_MACRO_BUSID_TYPE app_id_t;
        typedef atbus::protocol::msg msg_t;
        typedef std::shared_ptr<module_impl> module_ptr_t;

        struct flag_t {
            enum type {
                RUNNING = 0, //
                STOPING,     //
                TIMEOUT,
                IN_CALLBACK,
                RESET_TIMER,
                INITIALIZED,
                STOPPED,
                FLAG_MAX
            };
        };

        class flag_guard_t {
        public:
            LIBATAPP_MACRO_API flag_guard_t(app &owner, flag_t::type f);
            LIBATAPP_MACRO_API ~flag_guard_t();

        private:
            flag_guard_t(const flag_guard_t &);

            app *owner_;
            flag_t::type flag_;
        };
        friend class flag_guard_t;

        struct mode_t {
            enum type {
                CUSTOM = 0, // custom command
                START,      // start server
                STOP,       // send a stop command
                RELOAD,     // send a reload command
                INFO,       // show information and exit
                MODE_MAX
            };
        };

        struct custom_command_sender_t {
            app *self;
            std::list<std::string> *response;
        };

        // return > 0 means busy and will enter tick again as soon as possiable
        typedef std::function<int()> tick_handler_t;
        // parameters is (message head, buffer address, buffer size)
        typedef std::function<int(const msg_t &, const void *, size_t)> msg_handler_t;

        struct timer_info_t {
            bool is_activited;
            uv_timer_t timer;
        };

        struct tick_timer_t {
            util::time::time_utility::raw_time_t sec_update;
            time_t sec;
            time_t usec;

            timer_info_t tick_timer;
            timer_info_t timeout_timer;
        };


        typedef std::function<int(app &, const msg_t &, const void *, size_t)> callback_fn_on_msg_t;
        typedef std::function<int(app &, app_id_t src_pd, app_id_t dst_pd, const atbus::protocol::msg &m)> callback_fn_on_send_fail_t;
        typedef std::function<int(app &, atbus::endpoint &, int)> callback_fn_on_connected_t;
        typedef std::function<int(app &, atbus::endpoint &, int)> callback_fn_on_disconnected_t;
        typedef std::function<int(app &)> callback_fn_on_all_module_inited_t;

    public:
        LIBATAPP_MACRO_API app();
        LIBATAPP_MACRO_API ~app();

        /**
         * @brief run atapp loop until stop
         * @param ev_loop pointer to event loop
         * @param argc argument count for command line(include exec)
         * @param argv arguments for command line(include exec)
         * @param priv_data private data for custom option callbacks
         * @note you can call init(ev_loop, argc, argv, priv_data), and then call run(NULL, 0, NULL).
         * @return 0 or error code
         */
        LIBATAPP_MACRO_API int run(uv_loop_t *ev_loop, int argc, const char **argv, void *priv_data = NULL);

        /**
         * @brief initialize atapp
         * @param ev_loop pointer to event loop
         * @param argc argument count for command line(include exec)
         * @param argv arguments for command line(include exec)
         * @param priv_data private data for custom option callbacks
         * @return 0 or error code
         */
        LIBATAPP_MACRO_API int init(uv_loop_t *ev_loop, int argc, const char **argv, void *priv_data = NULL);

        /**
         * @brief run atapp loop but noblock if there is no event
         * @note you must call init(ev_loop, argc, argv, priv_data), before call run_noblock().
         * @param max_event_count max event in once call
         * @return 0 for no more actions or error code < 0 or 1 for there is pending actions
         */
        LIBATAPP_MACRO_API int run_noblock(uint64_t max_event_count = 20000);

        LIBATAPP_MACRO_API bool is_inited() const UTIL_CONFIG_NOEXCEPT;

        LIBATAPP_MACRO_API bool is_running() const UTIL_CONFIG_NOEXCEPT;

        LIBATAPP_MACRO_API bool is_closing() const UTIL_CONFIG_NOEXCEPT;

        LIBATAPP_MACRO_API bool is_closed() const UTIL_CONFIG_NOEXCEPT;

        LIBATAPP_MACRO_API int reload();

        LIBATAPP_MACRO_API int stop();

        LIBATAPP_MACRO_API int tick();

        LIBATAPP_MACRO_API app_id_t get_id() const;

        LIBATAPP_MACRO_API app_id_t convert_app_id_by_string(const char* id_in) const;

        LIBATAPP_MACRO_API bool check_flag(flag_t::type f) const;

        /**
         * @brief add a new module
         */
        LIBATAPP_MACRO_API void add_module(module_ptr_t module);

        /**
         * @brief convert module type and add a new module
         */
        template <typename TModPtr>
        LIBATAPP_MACRO_API_HEAD_ONLY void add_module(TModPtr module) {
            add_module(std::dynamic_pointer_cast<module_impl>(module));
        }

        // api: add custom log type
        // api: add custom module
        // api: add custom command callback
        LIBATAPP_MACRO_API util::cli::cmd_option_ci::ptr_type get_command_manager();

        // api: add custem program options
        LIBATAPP_MACRO_API util::cli::cmd_option::ptr_type get_option_manager();

        LIBATAPP_MACRO_API void set_app_version(const std::string &ver);

        LIBATAPP_MACRO_API const std::string &get_app_version() const;

        LIBATAPP_MACRO_API void set_build_version(const std::string &ver);

        LIBATAPP_MACRO_API const std::string &get_build_version() const;

        LIBATAPP_MACRO_API const std::string &get_app_name() const;

        LIBATAPP_MACRO_API const std::string &get_type_name() const;

        LIBATAPP_MACRO_API app_id_t get_type_id() const;

        LIBATAPP_MACRO_API const std::string &get_hash_code() const;

        LIBATAPP_MACRO_API std::shared_ptr<atbus::node> get_bus_node();
        LIBATAPP_MACRO_API const std::shared_ptr<atbus::node> get_bus_node() const;

        LIBATAPP_MACRO_API bool is_remote_address_available(const std::string &hostname, const std::string &address) const;

        LIBATAPP_MACRO_API util::config::ini_loader &get_configure();
        LIBATAPP_MACRO_API const util::config::ini_loader &get_configure() const;

        LIBATAPP_MACRO_API bool add_log_sink_maker(const std::string &name, log_sink_maker::log_reg_t fn);

        LIBATAPP_MACRO_API void set_evt_on_recv_msg(callback_fn_on_msg_t fn);
        LIBATAPP_MACRO_API void set_evt_on_send_fail(callback_fn_on_send_fail_t fn);
        LIBATAPP_MACRO_API void set_evt_on_app_connected(callback_fn_on_connected_t fn);
        LIBATAPP_MACRO_API void set_evt_on_app_disconnected(callback_fn_on_disconnected_t fn);
        LIBATAPP_MACRO_API void set_evt_on_all_module_inited(callback_fn_on_all_module_inited_t fn);

        LIBATAPP_MACRO_API const callback_fn_on_msg_t &get_evt_on_recv_msg() const;
        LIBATAPP_MACRO_API const callback_fn_on_send_fail_t &get_evt_on_send_fail() const;
        LIBATAPP_MACRO_API const callback_fn_on_connected_t &get_evt_on_app_connected() const;
        LIBATAPP_MACRO_API const callback_fn_on_disconnected_t &get_evt_on_app_disconnected() const;
        LIBATAPP_MACRO_API const callback_fn_on_all_module_inited_t &get_evt_on_all_module_inited() const;

    private:
        static void ev_stop_timeout(uv_timer_t *handle);

        bool set_flag(flag_t::type f, bool v);

        int apply_configure();

        void run_ev_loop(int run_mode);

        int run_inner(int run_mode);

        int setup_signal();

        void setup_option(int argc, const char *argv[], void *priv_data);

        void setup_command();

        int setup_log();

        int setup_atbus();

        void close_timer(timer_info_t &t);

        int setup_timer();

        int send_last_command(uv_loop_t *ev_loop);

        bool write_pidfile();
        bool cleanup_pidfile();
        void print_help();

        // ============ inner functional handlers ============

    public:
        static LIBATAPP_MACRO_API custom_command_sender_t get_custom_command_sender(util::cli::callback_param);
        static LIBATAPP_MACRO_API bool add_custom_command_rsp(util::cli::callback_param, const std::string &rsp_text);
        static LIBATAPP_MACRO_API void split_ids_by_string(const char* in, std::vector<app_id_t>& out);
        static LIBATAPP_MACRO_API app_id_t convert_app_id_by_string(const char* id_in, const std::vector<app_id_t>& mask_in);
        static LIBATAPP_MACRO_API app_id_t convert_app_id_by_string(const char* id_in, const char* mask_in);

    private:
        int prog_option_handler_help(util::cli::callback_param params, util::cli::cmd_option *opt_mgr, util::cli::cmd_option_ci *cmd_mgr);
        int prog_option_handler_version(util::cli::callback_param params);
        int prog_option_handler_set_id(util::cli::callback_param params);
        int prog_option_handler_set_id_mask(util::cli::callback_param params);
        int prog_option_handler_set_conf_file(util::cli::callback_param params);
        int prog_option_handler_set_pid(util::cli::callback_param params);
        int prog_option_handler_resume_mode(util::cli::callback_param params);
        int prog_option_handler_start(util::cli::callback_param params);
        int prog_option_handler_stop(util::cli::callback_param params);
        int prog_option_handler_reload(util::cli::callback_param params);
        int prog_option_handler_run(util::cli::callback_param params);

        int command_handler_start(util::cli::callback_param params);
        int command_handler_stop(util::cli::callback_param params);
        int command_handler_reload(util::cli::callback_param params);
        int command_handler_invalid(util::cli::callback_param params);

    private:
        int bus_evt_callback_on_recv_msg(const atbus::node &, const atbus::endpoint *, const atbus::connection *, const msg_t &, const void *, size_t);
        int bus_evt_callback_on_send_failed(const atbus::node &, const atbus::endpoint *, const atbus::connection *, const atbus::protocol::msg *m);
        int bus_evt_callback_on_error(const atbus::node &, const atbus::endpoint *, const atbus::connection *, int, int);
        int bus_evt_callback_on_reg(const atbus::node &, const atbus::endpoint *, const atbus::connection *, int);
        int bus_evt_callback_on_shutdown(const atbus::node &, int);
        int bus_evt_callback_on_available(const atbus::node &, int);
        int bus_evt_callback_on_invalid_connection(const atbus::node &, const atbus::connection *, int);
        int bus_evt_callback_on_custom_cmd(const atbus::node &, const atbus::endpoint *, const atbus::connection *, app_id_t,
                                           const std::vector<std::pair<const void *, size_t> > &, std::list<std::string> &);
        int bus_evt_callback_on_add_endpoint(const atbus::node &, atbus::endpoint *, int);
        int bus_evt_callback_on_remove_endpoint(const atbus::node &, atbus::endpoint *, int);
        int bus_evt_callback_on_custom_rsp(const atbus::node &, const atbus::endpoint *, const atbus::connection *, app_id_t,
                                           const std::vector<std::pair<const void *, size_t> > &, uint64_t);


        /** this function should always not be used outside atapp.cpp **/
        static void _app_setup_signal_term(int signo);

    private:
        static app *last_instance_;
        util::config::ini_loader cfg_loader_;
        util::cli::cmd_option::ptr_type app_option_;
        util::cli::cmd_option_ci::ptr_type cmd_handler_;
        std::vector<std::string> last_command_;
        int setup_result_;
        uint64_t last_proc_event_count_;

        app_conf conf_;
        mutable std::string build_version_;

        std::shared_ptr<atbus::node> bus_node_;
        std::bitset<flag_t::FLAG_MAX> flags_;
        mode_t::type mode_;
        tick_timer_t tick_timer_;

        std::vector<module_ptr_t> modules_;
        std::map<std::string, log_sink_maker::log_reg_t> log_reg_; // log reg will not changed or be checked outside the init, so std::map is enough

        // callbacks
        callback_fn_on_msg_t evt_on_recv_msg_;
        callback_fn_on_send_fail_t evt_on_send_fail_;
        callback_fn_on_connected_t evt_on_app_connected_;
        callback_fn_on_disconnected_t evt_on_app_disconnected_;
        callback_fn_on_all_module_inited_t evt_on_all_module_inited_;

        // stat
        typedef struct {
            uv_rusage_t last_checkpoint_usage;
            time_t last_checkpoint_min;
        } stat_data_t;
        stat_data_t stat_;
    };
} // namespace atapp

#endif
