// Copyright 2026 atframework

#include <atframe/atapp.h>
#include <atframe/atapp_module_impl.h>

#include <atbus_node.h>

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
static std::string get_atapp_setup_test_conf_path() {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  return conf_path_base + "/atapp_test_0.yaml";
}

class atapp_setup_test_init_success_module : public ::atframework::atapp::module_impl {
 public:
  int init_calls = 0;
  int stop_calls = 0;
  int timeout_calls = 0;
  int cleanup_calls = 0;

  const char *name() const override { return "atapp_setup_test_init_success_module"; }

  int init() override {
    ++init_calls;
    return 0;
  }

  int stop() override {
    ++stop_calls;
    return 0;
  }

  int timeout() override {
    ++timeout_calls;
    return 0;
  }

  void cleanup() override { ++cleanup_calls; }
};

class atapp_setup_test_never_init_module : public ::atframework::atapp::module_impl {
 public:
  int init_calls = 0;
  int stop_calls = 0;
  int cleanup_calls = 0;

  const char *name() const override { return "atapp_setup_test_never_init_module"; }

  int init() override {
    ++init_calls;
    return 0;
  }

  int stop() override {
    ++stop_calls;
    return 0;
  }

  void cleanup() override { ++cleanup_calls; }
};

class atapp_setup_test_init_failed_module : public ::atframework::atapp::module_impl {
 public:
  int init_calls = 0;
  int stop_calls = 0;
  int cleanup_calls = 0;
  int init_failed_stop_calls = 0;
  int init_failed_timeout_calls = 0;
  int init_failed_cleanup_calls = 0;

  atapp_setup_test_init_failed_module(int init_result, int stop_complete_after, int initialize_timeout_ms)
      : init_result_(init_result),
        stop_complete_after_(stop_complete_after),
        initialize_timeout_ms_(initialize_timeout_ms) {}

  const char *name() const override { return "atapp_setup_test_init_failed_module"; }

  int setup(::atframework::atapp::app_conf &conf) override {
    conf.origin.mutable_bus()->clear_listen();
    if (initialize_timeout_ms_ > 0) {
      conf.origin.mutable_timer()->mutable_initialize_timeout()->set_seconds(0);
      conf.origin.mutable_timer()->mutable_initialize_timeout()->set_nanos(initialize_timeout_ms_ * 1000000);
    }
    return 0;
  }

  int init() override {
    ++init_calls;
    return init_result_;
  }

  int stop() override {
    ++stop_calls;
    return 0;
  }

  void cleanup() override { ++cleanup_calls; }

  int init_failed_stop() override {
    ++init_failed_stop_calls;
    if (stop_complete_after_ <= 0) {
      return 1;
    }

    return init_failed_stop_calls >= stop_complete_after_ ? 0 : 1;
  }

  int init_failed_timeout() override {
    ++init_failed_timeout_calls;
    return 0;
  }

  void init_failed_cleanup() override { ++init_failed_cleanup_calls; }

 private:
  int init_result_;
  int stop_complete_after_;
  int initialize_timeout_ms_;
};

static atfw::util::log::log_level print_log_level = atfw::util::log::log_level::kDisabled;
}  // namespace

void atapp_unit_test_set_print_log_level(atfw::util::log::log_level level) { print_log_level = level; }

CASE_TEST(atapp_setup, init_failed_stop_cleanup_immediate) {
  std::string conf_path_1 = get_atapp_setup_test_conf_path();
  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};

  auto failed_module = std::make_shared<atapp_setup_test_init_failed_module>(-12345, 1, 0);
  auto later_module = std::make_shared<atapp_setup_test_never_init_module>();
  app1.add_module(failed_module);
  app1.add_module(later_module);

  CASE_EXPECT_EQ(-12345, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(1, failed_module->init_calls);
  CASE_EXPECT_EQ(1, failed_module->init_failed_stop_calls);
  CASE_EXPECT_EQ(0, failed_module->init_failed_timeout_calls);
  CASE_EXPECT_EQ(1, failed_module->init_failed_cleanup_calls);
  CASE_EXPECT_EQ(0, failed_module->stop_calls);
  CASE_EXPECT_EQ(0, failed_module->cleanup_calls);

  CASE_EXPECT_EQ(0, later_module->init_calls);
  CASE_EXPECT_EQ(0, later_module->stop_calls);
  CASE_EXPECT_EQ(0, later_module->cleanup_calls);
}

CASE_TEST(atapp_setup, init_failed_stop_cleanup_async_done) {
  std::string conf_path_1 = get_atapp_setup_test_conf_path();
  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};

  auto success_module = std::make_shared<atapp_setup_test_init_success_module>();
  auto failed_module = std::make_shared<atapp_setup_test_init_failed_module>(-12346, 3, 0);
  auto later_module = std::make_shared<atapp_setup_test_never_init_module>();
  app1.add_module(success_module);
  app1.add_module(failed_module);
  app1.add_module(later_module);

  CASE_EXPECT_EQ(-12346, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(1, success_module->init_calls);
  CASE_EXPECT_EQ(1, success_module->stop_calls);
  CASE_EXPECT_EQ(0, success_module->timeout_calls);
  CASE_EXPECT_EQ(1, success_module->cleanup_calls);

  CASE_EXPECT_EQ(1, failed_module->init_calls);
  CASE_EXPECT_EQ(3, failed_module->init_failed_stop_calls);
  CASE_EXPECT_EQ(0, failed_module->init_failed_timeout_calls);
  CASE_EXPECT_EQ(1, failed_module->init_failed_cleanup_calls);
  CASE_EXPECT_EQ(0, failed_module->stop_calls);
  CASE_EXPECT_EQ(0, failed_module->cleanup_calls);

  CASE_EXPECT_EQ(0, later_module->init_calls);
  CASE_EXPECT_EQ(0, later_module->stop_calls);
  CASE_EXPECT_EQ(0, later_module->cleanup_calls);
}

CASE_TEST(atapp_setup, init_failed_stop_cleanup_timeout) {
  std::string conf_path_1 = get_atapp_setup_test_conf_path();
  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app1;
  const char *args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};

  auto failed_module = std::make_shared<atapp_setup_test_init_failed_module>(-12347, 0, 100);
  auto later_module = std::make_shared<atapp_setup_test_never_init_module>();
  app1.add_module(failed_module);
  app1.add_module(later_module);

  CASE_EXPECT_EQ(-12347, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(1, failed_module->init_calls);
  CASE_EXPECT_GE(failed_module->init_failed_stop_calls, 1);
  CASE_EXPECT_EQ(1, failed_module->init_failed_timeout_calls);
  CASE_EXPECT_EQ(1, failed_module->init_failed_cleanup_calls);
  CASE_EXPECT_EQ(0, failed_module->stop_calls);
  CASE_EXPECT_EQ(0, failed_module->cleanup_calls);

  CASE_EXPECT_EQ(0, later_module->init_calls);
  CASE_EXPECT_EQ(0, later_module->stop_calls);
  CASE_EXPECT_EQ(0, later_module->cleanup_calls);
}

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

CASE_TEST(atapp_setup, match_gateway_scope_namespace_labels) {
  atframework::atapp::app app;
  app.set_metadata_scope("prod");
  app.set_metadata_namespace_name("game");
  app.set_metadata_label("zone", "east");

  // 空地址不可用
  {
    atapp::protocol::atapp_gateway gw;
    CASE_EXPECT_FALSE(app.match_gateway(gw));
  }

  // scope: Equal 语义, 空规则为通配
  {
    atapp::protocol::atapp_gateway gw;
    gw.set_address("ipv4://127.0.0.1:8001");
    CASE_EXPECT_TRUE(app.match_gateway(gw));
    gw.set_match_scope("prod");
    CASE_EXPECT_TRUE(app.match_gateway(gw));
    gw.set_match_scope("other");
    CASE_EXPECT_FALSE(app.match_gateway(gw));
  }

  // 应用未配置 scope 时, 带 scope 规则的地址不可达
  {
    atframework::atapp::app no_scope_app;
    atapp::protocol::atapp_gateway gw;
    gw.set_address("ipv4://127.0.0.1:8001");
    gw.set_match_scope("prod");
    CASE_EXPECT_FALSE(no_scope_app.match_gateway(gw));
    gw.set_match_scope("");
    CASE_EXPECT_TRUE(no_scope_app.match_gateway(gw));
  }

  // namespace: In 语义, 空项跳过
  {
    atapp::protocol::atapp_gateway gw;
    gw.set_address("ipv4://127.0.0.1:8001");
    gw.add_match_namespaces("");
    CASE_EXPECT_TRUE(app.match_gateway(gw));
    gw.add_match_namespaces("lobby");
    CASE_EXPECT_FALSE(app.match_gateway(gw));
    gw.add_match_namespaces("game");
    CASE_EXPECT_TRUE(app.match_gateway(gw));
  }

  // hosts: In 语义
  {
    atapp::protocol::atapp_gateway gw;
    gw.set_address("ipv4://127.0.0.1:8001");
    gw.add_match_hosts("atapp-test-no-such-host");
    CASE_EXPECT_FALSE(app.match_gateway(gw));
    gw.add_match_hosts(atbus::node::get_hostname());
    CASE_EXPECT_TRUE(app.match_gateway(gw));
  }

  // labels: Contains 语义, 缺失或不等都不可达
  {
    atapp::protocol::atapp_gateway gw;
    gw.set_address("ipv4://127.0.0.1:8001");
    (*gw.mutable_match_labels())["zone"] = "east";
    CASE_EXPECT_TRUE(app.match_gateway(gw));
    (*gw.mutable_match_labels())["zone"] = "west";
    CASE_EXPECT_FALSE(app.match_gateway(gw));
    (*gw.mutable_match_labels())["missing_key"] = "any";
    CASE_EXPECT_FALSE(app.match_gateway(gw));
  }

  // 组合规则: scope 匹配但 namespace 不匹配仍不可达
  {
    atapp::protocol::atapp_gateway gw;
    gw.set_address("ipv4://127.0.0.1:8001");
    gw.set_match_scope("prod");
    gw.add_match_namespaces("lobby");
    CASE_EXPECT_FALSE(app.match_gateway(gw));
  }
}

CASE_TEST(atapp_setup, atbus_isolation_configure_mapping) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_isolation_1.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app;
  const char *args[] = {"app", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app.init(nullptr, 4, args, nullptr));

  auto bus_node = app.get_bus_node();
  CASE_EXPECT_TRUE(bus_node != nullptr);
  if (!bus_node) {
    return;
  }

  // metadata 的 scope/namespace 必须下发到 atbus 节点配置
  const auto &bus_conf = bus_node->get_conf();
  CASE_EXPECT_EQ(std::string("prod"), bus_conf.scope);
  CASE_EXPECT_EQ(std::string("game"), bus_conf.namespace_name);

  // node_labels 只包含 inherited_labels 声明的标签
  CASE_EXPECT_EQ(static_cast<size_t>(2), bus_conf.node_labels.size());
  auto zone_iter = bus_conf.node_labels.find("zone");
  CASE_EXPECT_TRUE(zone_iter != bus_conf.node_labels.end() && zone_iter->second == "cn-east");
  auto env_iter = bus_conf.node_labels.find("deployment.environment");
  CASE_EXPECT_TRUE(env_iter != bus_conf.node_labels.end() && env_iter->second == "prod");
  CASE_EXPECT_TRUE(bus_conf.node_labels.end() == bus_conf.node_labels.find("not_inherited"));

  // bus.gateways 必须完整映射到 atbus gateway 配置
  CASE_EXPECT_EQ(static_cast<size_t>(2), bus_conf.gateway.size());
  if (bus_conf.gateway.size() >= 2) {
    const auto &gw0 = bus_conf.gateway[0];
    CASE_EXPECT_EQ(std::string("ipv4://10.0.0.1:21711"), gw0.address);
    CASE_EXPECT_EQ(std::string("prod"), gw0.match_scope);
    CASE_EXPECT_TRUE(gw0.match_hosts.end() != gw0.match_hosts.find("host-a"));
    CASE_EXPECT_TRUE(gw0.match_hosts.end() != gw0.match_hosts.find("host-b"));
    CASE_EXPECT_TRUE(gw0.match_namespaces.end() != gw0.match_namespaces.find("game"));
    // match_labels 同样只保留 inherited_labels 声明的标签
    CASE_EXPECT_EQ(static_cast<size_t>(1), gw0.match_labels.size());
    auto gw_label_iter = gw0.match_labels.find("zone");
    CASE_EXPECT_TRUE(gw_label_iter != gw0.match_labels.end() && gw_label_iter->second == "cn-east");
    CASE_EXPECT_TRUE(gw0.match_labels.end() == gw0.match_labels.find("not_inherited"));

    const auto &gw1 = bus_conf.gateway[1];
    CASE_EXPECT_EQ(std::string("ipv4://10.0.0.2:21711"), gw1.address);
    CASE_EXPECT_TRUE(gw1.match_scope.empty());
    CASE_EXPECT_TRUE(gw1.match_hosts.empty());
    CASE_EXPECT_TRUE(gw1.match_namespaces.empty());
    CASE_EXPECT_TRUE(gw1.match_labels.empty());
  }
}
