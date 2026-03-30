// Copyright 2026 atframework

#include <atframe/atapp.h>
#include <atframe/atapp_module_impl.h>

#include <common/file_system.h>
#include <log/log_wrapper.h>
#include <time/time_utility.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

#include "frame/test_macros.h"
#include "log/log_formatter.h"
#include "nostd/string_view.h"

class atapp_setup_test_timeout_module : public ::atframework::atapp::module_impl {
 public:
  int setup(::atframework::atapp::app_conf &conf) override {
    conf.origin.mutable_timer()->mutable_initialize_timeout()->set_seconds(1);
    return 0;
  }

  const char *name() const override { return "atapp_setup_test_timeout_module"; }

  int init() override {
    while (!get_app()->check_flag(::atframework::atapp::app::flag_t::kTimeout)) {
      get_app()->run_once(0, std::chrono::seconds{1});
    }

    // Success here should be redirect into EN_ATAPP_ERR_OPERATION_TIMEOUT by atframework::atapp::app
    return 0;
  }
};

CASE_TEST(atapp_setup, timeout) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_0.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};

  app1.add_module(std::make_shared<atapp_setup_test_timeout_module>());

  atfw::util::time::time_utility::update();
  auto before = atfw::util::time::time_utility::sys_now();
  CASE_EXPECT_EQ(atapp::EN_ATAPP_ERR_OPERATION_TIMEOUT, app1.init(nullptr, 4, args1, nullptr));
  atfw::util::time::time_utility::update();
  auto after = atfw::util::time::time_utility::sys_now();
  CASE_EXPECT_LE(std::chrono::duration_cast<std::chrono::seconds>(after - before).count(), 3);
}

namespace {
static atfw::util::log::log_level print_log_level = atfw::util::log::log_level::kDisabled;
}  // namespace

void atapp_unit_test_set_print_log_level(atfw::util::log::log_level level) { print_log_level = level; }

#if defined(_WIN32)
static int test_setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
static int test_unsetenv(const char *name) { return test_setenv(name, "", 1); }
#else
static int test_setenv(const char *name, const char *value, int overwrite) { return setenv(name, value, overwrite); }
static int test_unsetenv(const char *name) { return unsetenv(name); }
#endif

// Module to capture app_conf values during setup() for verification
class atapp_setup_test_capture_conf_module : public ::atframework::atapp::module_impl {
 public:
  std::string captured_id_cmd;
  std::vector<atbus::bus_id_t> captured_id_mask;
  std::string captured_conf_file;
  std::string captured_pid_file;
  std::string captured_start_error_file;
  std::list<std::string> captured_startup_log;

  int setup(::atframework::atapp::app_conf &conf) override {
    captured_id_cmd = conf.id_cmd;
    captured_id_mask = conf.id_mask;
    captured_conf_file = conf.conf_file;
    captured_pid_file = conf.pid_file;
    captured_start_error_file = conf.start_error_file;
    captured_startup_log = conf.startup_log;
    return 0;
  }

  const char *name() const override { return "atapp_setup_test_capture_conf_module"; }

  int init() override { return 0; }
};

CASE_TEST(atapp_setup, prog_option_expand_environment_expression) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_0.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  // Set up environment variables for expression expansion
  test_setenv("ATAPP_TEST_OPT_ID", "0x12345678", 1);
  test_setenv("ATAPP_TEST_OPT_ID_MASK", "8.8.8.8", 1);
  test_setenv("ATAPP_TEST_OPT_PID_DIR", "/tmp/test_pid", 1);
  test_setenv("ATAPP_TEST_OPT_PID_NAME", "myapp", 1);
  test_setenv("ATAPP_TEST_OPT_ERR_FILE", "startup_errors.log", 1);
  test_setenv("ATAPP_TEST_OPT_LOG_FILE", "startup.log", 1);

  auto capture_module = std::make_shared<atapp_setup_test_capture_conf_module>();

  {
    atframework::atapp::app app1;
    app1.add_module(capture_module);

    // Use environment variable expressions in command-line arguments
    const char *args1[] = {"app1",
                           "-id",
                           "$ATAPP_TEST_OPT_ID",
                           "-id-mask",
                           "${ATAPP_TEST_OPT_ID_MASK}",
                           "-p",
                           "${ATAPP_TEST_OPT_PID_DIR}/${ATAPP_TEST_OPT_PID_NAME}.pid",
                           "--startup-error-file",
                           "${ATAPP_TEST_OPT_ERR_FILE:-default_error.log}",
                           "--startup-log",
                           "${ATAPP_TEST_OPT_LOG_FILE}",
                           "-c",
                           conf_path_1.c_str(),
                           "start"};

    // init will fail because bus listen port may conflict, but command-line parsing should succeed
    app1.init(nullptr, static_cast<int>(sizeof(args1) / sizeof(args1[0])), args1, nullptr);

    // Verify id_cmd was expanded
    CASE_EXPECT_EQ("0x12345678", capture_module->captured_id_cmd);
    CASE_MSG_INFO() << "id_cmd = " << capture_module->captured_id_cmd << std::endl;

    // Verify id_mask was expanded
    CASE_EXPECT_FALSE(capture_module->captured_id_mask.empty());
    if (!capture_module->captured_id_mask.empty()) {
      // 8.8.8.8 should produce valid mask values
      CASE_MSG_INFO() << "id_mask size = " << capture_module->captured_id_mask.size() << std::endl;
    }

    // Verify pid_file was expanded with nested env vars
    CASE_EXPECT_EQ("/tmp/test_pid/myapp.pid", capture_module->captured_pid_file);
    CASE_MSG_INFO() << "pid_file = " << capture_module->captured_pid_file << std::endl;

    // Verify start_error_file was expanded (with default value syntax)
    CASE_EXPECT_EQ("startup_errors.log", capture_module->captured_start_error_file);
    CASE_MSG_INFO() << "start_error_file = " << capture_module->captured_start_error_file << std::endl;

    // Verify startup_log was expanded
    CASE_EXPECT_FALSE(capture_module->captured_startup_log.empty());
    if (!capture_module->captured_startup_log.empty()) {
      CASE_EXPECT_EQ("startup.log", capture_module->captured_startup_log.front());
      CASE_MSG_INFO() << "startup_log[0] = " << capture_module->captured_startup_log.front() << std::endl;
    }

    // Verify conf_file was NOT expanded (it's the actual path, not an expression)
    CASE_EXPECT_EQ(conf_path_1, capture_module->captured_conf_file);
  }

  // Clean up environment variables
  test_unsetenv("ATAPP_TEST_OPT_ID");
  test_unsetenv("ATAPP_TEST_OPT_ID_MASK");
  test_unsetenv("ATAPP_TEST_OPT_PID_DIR");
  test_unsetenv("ATAPP_TEST_OPT_PID_NAME");
  test_unsetenv("ATAPP_TEST_OPT_ERR_FILE");
  test_unsetenv("ATAPP_TEST_OPT_LOG_FILE");
}

CASE_TEST(atapp_setup, prog_option_expand_environment_expression_with_default) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_0.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  // Ensure these variables are NOT set so defaults are used
  test_unsetenv("ATAPP_TEST_OPT_UNSET_ID");
  test_unsetenv("ATAPP_TEST_OPT_UNSET_PID");
  test_unsetenv("ATAPP_TEST_OPT_UNSET_ERR");

  auto capture_module = std::make_shared<atapp_setup_test_capture_conf_module>();

  {
    atframework::atapp::app app1;
    app1.add_module(capture_module);

    // Use default value syntax for unset environment variables
    const char *args1[] = {"app1",
                           "-id",
                           "${ATAPP_TEST_OPT_UNSET_ID:-0xAABBCCDD}",
                           "-p",
                           "${ATAPP_TEST_OPT_UNSET_PID:-/var/run/default.pid}",
                           "--startup-error-file",
                           "${ATAPP_TEST_OPT_UNSET_ERR:-/tmp/default_error.log}",
                           "-c",
                           conf_path_1.c_str(),
                           "start"};

    app1.init(nullptr, static_cast<int>(sizeof(args1) / sizeof(args1[0])), args1, nullptr);

    // Verify default values were used for unset variables
    CASE_EXPECT_EQ("0xAABBCCDD", capture_module->captured_id_cmd);
    CASE_MSG_INFO() << "id_cmd (default) = " << capture_module->captured_id_cmd << std::endl;

    CASE_EXPECT_EQ("/var/run/default.pid", capture_module->captured_pid_file);
    CASE_MSG_INFO() << "pid_file (default) = " << capture_module->captured_pid_file << std::endl;

    CASE_EXPECT_EQ("/tmp/default_error.log", capture_module->captured_start_error_file);
    CASE_MSG_INFO() << "start_error_file (default) = " << capture_module->captured_start_error_file << std::endl;
  }
}

CASE_TEST(atapp_setup, prog_option_no_expression_passthrough) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_0.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  auto capture_module = std::make_shared<atapp_setup_test_capture_conf_module>();

  {
    atframework::atapp::app app1;
    app1.add_module(capture_module);

    // Use plain values without any expressions — should pass through unchanged
    const char *args1[] = {"app1",
                           "-id",
                           "0x00001234",
                           "-p",
                           "/tmp/plain.pid",
                           "--startup-error-file",
                           "/tmp/plain_error.log",
                           "-c",
                           conf_path_1.c_str(),
                           "start"};

    app1.init(nullptr, static_cast<int>(sizeof(args1) / sizeof(args1[0])), args1, nullptr);

    CASE_EXPECT_EQ("0x00001234", capture_module->captured_id_cmd);
    CASE_EXPECT_EQ("/tmp/plain.pid", capture_module->captured_pid_file);
    CASE_EXPECT_EQ("/tmp/plain_error.log", capture_module->captured_start_error_file);
  }
}

CASE_TEST_EVENT_ON_START(unit_test_event_on_start_setup_logger) {
  auto *default_cat =
      atfw::util::log::log_wrapper::mutable_log_cat(atfw::util::log::log_wrapper::categorize_t::DEFAULT);
  if (default_cat) {
    default_cat->add_sink(
        [](const atfw::util::log::log_formatter::caller_info_t &caller, atfw::util::nostd::string_view content) {
          if (static_cast<int>(caller.level_id) >= static_cast<int>(print_log_level)) {
            std::cout.write(content.data(), static_cast<std::streamsize>(content.size()));
            std::cout << '\n';
          }
        });
  }
}
