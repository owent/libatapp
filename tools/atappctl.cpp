
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include <uv.h>

#include <atframe/atapp.h>
#include <common/file_system.h>

#include <libatbus.h>
#include <libatbus_protocol.h>
#include <time/time_utility.h>

static int exit_code = 0;

static void _log_sink_stdout_handle(const util::log::log_wrapper::caller_info_t &, const char *content, size_t content_size) {
    std::cout.write(content, content_size);
    std::cout << std::endl;
}

class atappctl_module : public atapp::module_impl {
public:
    virtual int init() {
        WLOG_GETCAT(util::log::log_wrapper::categorize_t::DEFAULT)->add_sink(_log_sink_stdout_handle);
        WLOG_GETCAT(util::log::log_wrapper::categorize_t::DEFAULT)
            ->set_stacktrace_level(util::log::log_formatter::level_t::LOG_LW_DISABLED, util::log::log_formatter::level_t::LOG_LW_DISABLED);
        return 0;
    };

    virtual int reload() { return 0; }

    virtual int stop() { return 0; }

    virtual int timeout() { return 0; }

    virtual const char *name() const { return "atappctl_module"; }

    virtual int tick() { return 0; }
};

static int app_handle_on_msg(atapp::app &, const atapp::app::msg_t &msg, const void *buffer, size_t len) {
    std::string data;
    data.assign(reinterpret_cast<const char *>(buffer), len);
    WLOGINFO("receive a message(from 0x%llx, type=%d) %s", static_cast<unsigned long long>(msg.head().src_bus_id()), msg.head().type(), data.c_str());
    return 0;
}

static int app_handle_on_send_fail(atapp::app &, atapp::app::app_id_t, atapp::app::app_id_t dst_pd, const atbus::protocol::msg &) {
    WLOGERROR("send data to 0x%llx failed", static_cast<unsigned long long>(dst_pd));
    exit_code = 1;
    return 0;
}

static int app_handle_on_connected(atapp::app &, atbus::endpoint &ep, int status) {
    WLOGINFO("app 0x%llx connected, status: %d", static_cast<unsigned long long>(ep.get_id()), status);
    return 0;
}

static int app_handle_on_disconnected(atapp::app &, atbus::endpoint &ep, int status) {
    WLOGINFO("app 0x%llx disconnected, status: %d", static_cast<unsigned long long>(ep.get_id()), status);
    return 0;
}

int main(int argc, char *argv[]) {
    atapp::app app;

    // project directory
    {
        std::string proj_dir;
        util::file_system::dirname(__FILE__, 0, proj_dir, 2);
        util::log::log_formatter::set_project_directory(proj_dir.c_str(), proj_dir.size());
    }

    // setup module
    app.add_module(std::make_shared<atappctl_module>());

    // setup handle
    app.set_evt_on_recv_msg(app_handle_on_msg);
    app.set_evt_on_forward_response(app_handle_on_send_fail);
    app.set_evt_on_app_connected(app_handle_on_connected);
    app.set_evt_on_app_disconnected(app_handle_on_disconnected);

    // run
    int ret = app.run(uv_default_loop(), argc, (const char **)argv, NULL);
    if (0 == ret) {
        return exit_code;
    }

    return ret;
}
