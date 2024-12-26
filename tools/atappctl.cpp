
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

static void _log_sink_stdout_handle(const atfw::util::log::log_wrapper::caller_info_t &, const char *content,
                                    size_t content_size) {
  std::cout.write(content, content_size);
  std::cout << std::endl;
}

class atappctl_module : public atapp::module_impl {
 public:
  int init() override {
    WLOG_GETCAT(atfw::util::log::log_wrapper::categorize_t::DEFAULT)->add_sink(_log_sink_stdout_handle);
    WLOG_GETCAT(atfw::util::log::log_wrapper::categorize_t::DEFAULT)
        ->set_stacktrace_level(atfw::util::log::log_formatter::level_t::LOG_LW_DISABLED,
                               atfw::util::log::log_formatter::level_t::LOG_LW_DISABLED);
    return 0;
  };

  int reload() override { return 0; }

  int stop() override { return 0; }

  int timeout() override { return 0; }

  const char *name() const override { return "atappctl_module"; }

  int tick() override { return 0; }
};

static int app_handle_on_msg(atapp::app &, const atapp::app::message_sender_t &source,
                             const atapp::app::message_t &msg) {
  std::string data;
  data.assign(reinterpret_cast<const char *>(msg.data), msg.data_size);
  FWLOGINFO("receive a message(from {:#x}, type={}) {}", source.id, msg.type, data);
  return 0;
}

static int app_handle_on_response(atapp::app &, const atapp::app::message_sender_t &source,
                                  const atapp::app::message_t &, int32_t error_code) {
  if (error_code < 0) {
    FWLOGERROR("send data to {:#x} failed, code: {}", source.id, error_code);
  } else {
    FWLOGINFO("send data to {:#x} finished", source.id);
  }
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
    atfw::util::file_system::dirname(__FILE__, 0, proj_dir, 2);
    atfw::util::log::log_formatter::set_project_directory(proj_dir.c_str(), proj_dir.size());
  }

  // setup module
  app.add_module(std::make_shared<atappctl_module>());

  // setup handle
  app.set_evt_on_forward_request(app_handle_on_msg);
  app.set_evt_on_forward_response(app_handle_on_response);
  app.set_evt_on_app_connected(app_handle_on_connected);
  app.set_evt_on_app_disconnected(app_handle_on_disconnected);

  // run
  int ret = app.run(uv_default_loop(), argc, (const char **)argv, nullptr);
  if (0 == ret) {
    return exit_code;
  }

  return ret;
}
