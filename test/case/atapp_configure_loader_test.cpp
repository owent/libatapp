// Copyright 2026 atframework

#include <atframe/atapp.h>

#include <common/file_system.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "frame/test_macros.h"

#if defined(_WIN32)
inline int setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
inline int unsetenv(const char *name) { return setenv(name, "", 1); }
#endif

static void check_origin_configure(atframework::atapp::app &app, atapp::protocol::atapp_etcd sub_cfg,
                                   const atapp::configure_key_set &existed_app_keys,
                                   const atapp::configure_key_set &existed_etcd_keys) {
  CASE_EXPECT_EQ(app.get_id(), 0x1234);
  CASE_EXPECT_EQ(app.get_origin_configure().id_mask(), "8.8.8.8");
  CASE_EXPECT_EQ(app.convert_app_id_by_string("1.2.3.4"), 0x01020304);

  CASE_EXPECT_EQ(1, app.get_origin_configure().bus().listen_size());
  CASE_EXPECT_EQ("atcp://:::21437", app.get_origin_configure().bus().listen(0));

  CASE_EXPECT_EQ(30, app.get_origin_configure().bus().first_idle_timeout().seconds());
  CASE_EXPECT_EQ(60, app.get_origin_configure().bus().ping_interval().seconds());
  CASE_EXPECT_EQ(3, app.get_origin_configure().bus().retry_interval().seconds());
  CASE_EXPECT_EQ(3, app.get_origin_configure().bus().fault_tolerant());
  CASE_EXPECT_EQ(262144, app.get_origin_configure().bus().message_size());
  CASE_EXPECT_EQ(8388608, app.get_origin_configure().bus().receive_buffer_size());
  CASE_EXPECT_EQ(2097152, app.get_origin_configure().bus().send_buffer_size());
  CASE_EXPECT_EQ(0, app.get_origin_configure().bus().send_buffer_number());
  CASE_EXPECT_EQ(1000, app.get_origin_configure().bus().loop_times());
  CASE_EXPECT_EQ(16, app.get_origin_configure().bus().ttl());

  CASE_EXPECT_EQ(0, app.get_origin_configure().timer().tick_interval().seconds());
  CASE_EXPECT_EQ(32000000, app.get_origin_configure().timer().tick_interval().nanos());
  CASE_EXPECT_EQ(10, app.get_origin_configure().timer().stop_timeout().seconds());
  CASE_EXPECT_EQ(0, app.get_origin_configure().timer().stop_timeout().nanos());
  CASE_EXPECT_EQ(0, app.get_origin_configure().timer().stop_interval().seconds());
  CASE_EXPECT_EQ(256000000, app.get_origin_configure().timer().stop_interval().nanos());
  CASE_EXPECT_EQ(8, app.get_origin_configure().timer().message_timeout().seconds());
  CASE_EXPECT_EQ(10, app.get_origin_configure().timer().initialize_timeout().seconds());

  CASE_EXPECT_EQ(3, app.get_origin_configure().etcd().hosts_size());
  CASE_EXPECT_EQ("http://127.0.0.1:2375", app.get_origin_configure().etcd().hosts(0));
  CASE_EXPECT_EQ("http://127.0.0.1:2376", app.get_origin_configure().etcd().hosts(1));
  CASE_EXPECT_EQ("http://127.0.0.1:2377", app.get_origin_configure().etcd().hosts(2));
  CASE_EXPECT_EQ("/atapp/services/astf4g/", app.get_origin_configure().etcd().path());
  CASE_EXPECT_EQ("", app.get_origin_configure().etcd().authorization());
  CASE_EXPECT_EQ(0, app.get_origin_configure().etcd().init().tick_interval().seconds());
  CASE_EXPECT_EQ(16000000, app.get_origin_configure().etcd().init().tick_interval().nanos());
  CASE_EXPECT_EQ(10, app.get_origin_configure().etcd().init().timeout().seconds());
  CASE_EXPECT_EQ(0, app.get_origin_configure().etcd().init().timeout().nanos());
  CASE_EXPECT_TRUE(app.get_origin_configure().etcd().cluster().auto_update());

  CASE_EXPECT_EQ(3, sub_cfg.hosts_size());
  if (3 == sub_cfg.hosts_size()) {
    CASE_EXPECT_EQ("http://127.0.0.1:2375", sub_cfg.hosts(0));
    CASE_EXPECT_EQ("http://127.0.0.1:2376", sub_cfg.hosts(1));
    CASE_EXPECT_EQ("http://127.0.0.1:2377", sub_cfg.hosts(2));
  }
  CASE_EXPECT_EQ("/atapp/services/astf4g/", sub_cfg.path());
  CASE_EXPECT_EQ("", sub_cfg.authorization());
  CASE_EXPECT_EQ(0, sub_cfg.init().tick_interval().seconds());
  CASE_EXPECT_EQ(16000000, sub_cfg.init().tick_interval().nanos());
  CASE_EXPECT_EQ(10, sub_cfg.init().timeout().seconds());
  CASE_EXPECT_EQ(0, sub_cfg.init().timeout().nanos());
  CASE_EXPECT_TRUE(sub_cfg.cluster().auto_update());

  {
    auto map_kv1 = app.get_origin_configure().metadata().labels().find("deployment.environment");
    auto map_kv2 = app.get_origin_configure().metadata().labels().find("deployment.region");
    CASE_EXPECT_TRUE(map_kv1 != app.get_origin_configure().metadata().labels().end());
    CASE_EXPECT_TRUE(map_kv2 != app.get_origin_configure().metadata().labels().end());
    if (map_kv1 != app.get_origin_configure().metadata().labels().end()) {
      CASE_EXPECT_EQ("test", map_kv1->second);
    }
    if (map_kv2 != app.get_origin_configure().metadata().labels().end()) {
      CASE_EXPECT_EQ("cn", map_kv2->second);
    }
  }

  // Check etcd keys
  {
    std::string keys_path;
    atfw::util::file_system::dirname(__FILE__, 0, keys_path);
    keys_path += "/atapp_configure_loader_test_etcd_keys.txt";

    if (!atfw::util::file_system::is_exist(keys_path.c_str())) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << keys_path << " not found, skip checking etcd keys" << '\n';
      return;
    }

    std::fstream keys_ss(keys_path.c_str(), std::ios::in);
    std::string key_line;
    size_t keys_size = 0;
    while (std::getline(keys_ss, key_line)) {
      auto trimed_line = atfw::util::string::trim(key_line.c_str(), key_line.size());
      if (trimed_line.second == 0) {
        continue;
      }

      ++keys_size;
      bool check_exists =
          existed_etcd_keys.end() != existed_etcd_keys.find(std::string(trimed_line.first, trimed_line.second));
      if (!check_exists) {
        CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << std::string(trimed_line.first, trimed_line.second) << " not found"
                        << '\n';
      }
      CASE_EXPECT_TRUE(check_exists);
    }
    CASE_EXPECT_LE(keys_size, existed_etcd_keys.size());
  }

  // Check app keys
  {
    std::string keys_path;
    atfw::util::file_system::dirname(__FILE__, 0, keys_path);
    keys_path += "/atapp_configure_loader_test_app_keys.txt";

    if (!atfw::util::file_system::is_exist(keys_path.c_str())) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << keys_path << " not found, skip checking etcd keys" << '\n';
      return;
    }

    std::fstream keys_ss(keys_path.c_str(), std::ios::in);
    std::string key_line;
    size_t keys_size = 0;
    while (std::getline(keys_ss, key_line)) {
      auto trimed_line = atfw::util::string::trim(key_line.c_str(), key_line.size());
      if (trimed_line.second == 0) {
        continue;
      }

      ++keys_size;
      bool check_exists =
          existed_app_keys.end() != existed_app_keys.find(std::string(trimed_line.first, trimed_line.second));
      if (!check_exists) {
        CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << std::string(trimed_line.first, trimed_line.second) << " not found"
                        << '\n';
      }
      CASE_EXPECT_TRUE(check_exists);
    }
    CASE_EXPECT_LE(keys_size, existed_app_keys.size());
  }
}

static void check_log_configure(const atapp::protocol::atapp_log &app_log,
                                const atapp::configure_key_set &existed_keys) {
  CASE_EXPECT_EQ(std::string("debug"), app_log.level());
  CASE_EXPECT_EQ(2, app_log.category_size());
  if (app_log.category_size() < 2) {
    return;
  }

  CASE_EXPECT_EQ(std::string("default"), app_log.category(0).name());
  CASE_EXPECT_EQ(4, app_log.category(0).sink_size());
  CASE_EXPECT_EQ(0, app_log.category(1).sink_size());
  CASE_EXPECT_EQ(std::string("db"), app_log.category(1).name());

  if (app_log.category(0).sink_size() < 4) {
    return;
  }
  CASE_EXPECT_EQ(std::string("file"), app_log.category(0).sink(0).type());
  CASE_EXPECT_EQ(atapp::protocol::atapp_log_sink::kLogBackendFile, app_log.category(0).sink(0).backend_case());
  CASE_EXPECT_EQ(std::string("stdout"), app_log.category(0).sink(3).type());
  CASE_EXPECT_EQ(atapp::protocol::atapp_log_sink::kLogBackendStdout, app_log.category(0).sink(3).backend_case());

  // Check app keys
  std::string keys_path;
  atfw::util::file_system::dirname(__FILE__, 0, keys_path);
  keys_path += "/atapp_configure_loader_test_log_keys.txt";

  if (!atfw::util::file_system::is_exist(keys_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << keys_path << " not found, skip checking etcd keys" << '\n';
    return;
  }

  std::fstream keys_ss(keys_path.c_str(), std::ios::in);
  std::string key_line;
  size_t keys_size = 0;
  while (std::getline(keys_ss, key_line)) {
    auto trimed_line = atfw::util::string::trim(key_line.c_str(), key_line.size());
    if (trimed_line.second == 0) {
      continue;
    }

    ++keys_size;
    bool check_exists = existed_keys.end() != existed_keys.find(std::string(trimed_line.first, trimed_line.second));
    if (!check_exists) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << std::string(trimed_line.first, trimed_line.second) << " not found"
                      << '\n';
    }
    CASE_EXPECT_TRUE(check_exists);
  }
  CASE_EXPECT_LE(keys_size, existed_keys.size());
}

CASE_TEST(atapp_configure, load_yaml) {
  atframework::atapp::app app;
  std::string conf_path;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_loader_test.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.load_yaml" << '\n';
    return;
  }

  const char *argv[] = {"unit-test", "-c", &conf_path[0], "--version"};
  app.init(nullptr, 4, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::configure_key_set existed_etcd_keys;
  atapp::configure_key_set existed_log_keys;
  atapp::protocol::atapp_configure app_cfg;
  app.parse_configures_into(app_cfg, "atapp", "", &existed_app_keys);
  atapp::protocol::atapp_etcd etcd_cfg;
  app.parse_configures_into(etcd_cfg, "atapp.etcd", "", &existed_etcd_keys);
  check_origin_configure(app, etcd_cfg, existed_app_keys, existed_etcd_keys);

  atapp::protocol::atapp_log app_log;
  app.parse_log_configures_into(app_log, std::vector<gsl::string_view>{"atapp", "log"}, "", &existed_log_keys);
  check_log_configure(app_log, existed_log_keys);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();
}

CASE_TEST(atapp_configure, load_conf) {
  atframework::atapp::app app;
  std::string conf_path;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_loader_test.conf";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.load_conf" << '\n';
    return;
  }

  const char *argv[] = {"unit-test", "-c", &conf_path[0], "--version"};
  app.init(nullptr, 4, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::configure_key_set existed_etcd_keys;
  atapp::configure_key_set existed_log_keys;
  atapp::protocol::atapp_configure app_cfg;
  app.parse_configures_into(app_cfg, "atapp", "", &existed_app_keys);
  atapp::protocol::atapp_etcd etcd_cfg;
  app.parse_configures_into(etcd_cfg, "atapp.etcd", "", &existed_etcd_keys);
  check_origin_configure(app, etcd_cfg, existed_app_keys, existed_etcd_keys);

  atapp::protocol::atapp_log app_log;
  app.parse_log_configures_into(app_log, std::vector<gsl::string_view>{"atapp", "log"}, "", &existed_log_keys);
  check_log_configure(app_log, existed_log_keys);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();
}

CASE_TEST(atapp_configure, load_environment) {
  atframework::atapp::app app;
  std::string conf_path;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_loader_test.env.txt";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.load_environment"
                    << '\n';
    return;
  }

  std::fstream fs(conf_path.c_str(), std::ios::in);
  std::string line;
  std::unordered_set<std::string> env_vars;
  while (std::getline(fs, line)) {
    auto trimed_line = atfw::util::string::trim(line.c_str(), line.size());
    if (trimed_line.second == 0) {
      continue;
    }

    auto equal_index = std::find(trimed_line.first, trimed_line.first + trimed_line.second, '=');
    if (equal_index == trimed_line.first + trimed_line.second) {
      continue;
    }

    std::string key{trimed_line.first, equal_index};
    std::string value{equal_index + 1, trimed_line.first + trimed_line.second};
    env_vars.insert(key);
    setenv(key.c_str(), value.c_str(), 1);
  }

  const char *argv[] = {"unit-test", "--version"};
  app.init(nullptr, 2, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::configure_key_set existed_etcd_keys;
  atapp::configure_key_set existed_log_keys;
  atapp::protocol::atapp_configure app_cfg;
  app.parse_configures_into(app_cfg, "atapp", "ATAPP", &existed_app_keys);
  atapp::protocol::atapp_etcd etcd_cfg;
  app.parse_configures_into(etcd_cfg, "atapp.etcd", "ATAPP_ETCD", &existed_etcd_keys);
  check_origin_configure(app, etcd_cfg, existed_app_keys, existed_etcd_keys);

  atapp::protocol::atapp_log app_log;
  app.parse_log_configures_into(app_log, std::vector<gsl::string_view>{"atapp", "log"}, "ATAPP_LOG", &existed_log_keys);
  check_log_configure(app_log, existed_log_keys);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();

  for (auto &env_var : env_vars) {
    unsetenv(env_var.c_str());
  }
}

// =============================================================================
// Expression expansion tests
// =============================================================================

// Test expand_environment_expression() public API directly
CASE_TEST(atapp_configure, expression_expand_direct) {
  // Set up environment variables
  setenv("ATAPP_EXPR_DIRECT_NAME", "my_app", 1);
  setenv("ATAPP_EXPR_DIRECT_HOST", "server01.example.com", 1);
  setenv("ATAPP_EXPR_DIRECT_SUFFIX", "PROD", 1);
  setenv("ATAPP_EXPR_DIRECT_NESTED_PROD", "production_value", 1);
  setenv("ATAPP_EXPR_DIRECT_OUTER", "outer_val", 1);
  setenv("ATAPP_EXPR_DIRECT_INNER", "inner_val", 1);
  setenv("ATAPP_EXPR_DIRECT_EMPTY", "", 1);
  // k8s-style variable names with dots, hyphens, slashes
  setenv("app.kubernetes.io/name", "k8s_app", 1);
  setenv("deployment.environment-name", "staging", 1);
  setenv("area.region", "cn-east-1", 1);

  // 1. Simple $VAR reference
  {
    auto result = atapp::expand_environment_expression("$ATAPP_EXPR_DIRECT_NAME");
    CASE_EXPECT_EQ("my_app", result);
  }

  // 2. Braced ${VAR} reference
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_HOST}");
    CASE_EXPECT_EQ("server01.example.com", result);
  }

  // 3. Mixed text and variable
  {
    auto result = atapp::expand_environment_expression("prefix_${ATAPP_EXPR_DIRECT_NAME}_suffix");
    CASE_EXPECT_EQ("prefix_my_app_suffix", result);
  }

  // 4. Multiple variables in one string
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_NAME}@${ATAPP_EXPR_DIRECT_HOST}");
    CASE_EXPECT_EQ("my_app@server01.example.com", result);
  }

  // 5. ${VAR:-default} with existing variable
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_NAME:-fallback}");
    CASE_EXPECT_EQ("my_app", result);
  }

  // 6. ${VAR:-default} with missing variable — should use default
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_UNDEFINED_VAR:-fallback_value}");
    CASE_EXPECT_EQ("fallback_value", result);
  }

  // 7. ${VAR:-default} with empty variable — follows bash ":-" semantics: empty triggers default
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_EMPTY:-fallback_value}");
    // empty env var is treated as unset for :- operator (bash semantics)
    CASE_EXPECT_EQ("fallback_value", result);
  }

  // 8. ${VAR:+word} with existing variable — should use word
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_NAME:+replacement_word}");
    CASE_EXPECT_EQ("replacement_word", result);
  }

  // 9. ${VAR:+word} with missing variable — should be empty
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_UNDEFINED_VAR:+should_not_appear}");
    CASE_EXPECT_EQ("", result);
  }

  // 10. \$ escape — should produce literal $
  {
    auto result = atapp::expand_environment_expression("price\\$100");
    CASE_EXPECT_EQ("price$100", result);
  }

  // 11. \$ escape at end of string
  {
    auto result = atapp::expand_environment_expression("total\\$");
    CASE_EXPECT_EQ("total$", result);
  }

  // 12. Nested expression: ${OUTER_${INNER}}
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_NESTED_${ATAPP_EXPR_DIRECT_SUFFIX}}");
    CASE_EXPECT_EQ("production_value", result);
  }

  // 13. Multi-level nesting: ${VAR:-${OTHER:-default2}}
  {
    auto result =
        atapp::expand_environment_expression("${ATAPP_EXPR_UNDEFINED_VAR:-${ATAPP_EXPR_DIRECT_NAME:-default2}}");
    CASE_EXPECT_EQ("my_app", result);
  }

  // 14. Multi-level nesting with both undefined: ${VAR:-${OTHER:-default2}}
  {
    auto result =
        atapp::expand_environment_expression("${ATAPP_EXPR_UNDEFINED_VAR:-${ATAPP_EXPR_UNDEFINED_VAR2:-deep_default}}");
    CASE_EXPECT_EQ("deep_default", result);
  }

  // 15. Undefined variable with no default — should be empty
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_UNDEFINED_VAR}");
    CASE_EXPECT_EQ("", result);
  }

  // 16. Undefined bare variable — should be empty
  {
    auto result = atapp::expand_environment_expression("$ATAPP_EXPR_UNDEFINED_VAR");
    CASE_EXPECT_EQ("", result);
  }

  // 17. No expressions — passthrough
  {
    auto result = atapp::expand_environment_expression("plain text with no expressions");
    CASE_EXPECT_EQ("plain text with no expressions", result);
  }

  // 18. Empty string
  {
    auto result = atapp::expand_environment_expression("");
    CASE_EXPECT_EQ("", result);
  }

  // 19. Nested ${VAR:+word} — word contains expression
  {
    auto result = atapp::expand_environment_expression("${ATAPP_EXPR_DIRECT_NAME:+found_${ATAPP_EXPR_DIRECT_SUFFIX}}");
    CASE_EXPECT_EQ("found_PROD", result);
  }

  // 20. Combined: text + escape + variable + default
  {
    auto result = atapp::expand_environment_expression(
        "host=${ATAPP_EXPR_DIRECT_HOST},port\\$=${ATAPP_EXPR_UNDEFINED_PORT:-8080}");
    CASE_EXPECT_EQ("host=server01.example.com,port$=8080", result);
  }

  // 21. k8s label-style variable name with dots and slashes in braced form
  {
    auto result = atapp::expand_environment_expression("${app.kubernetes.io/name}");
    CASE_EXPECT_EQ("k8s_app", result);
  }

  // 22. k8s label-style variable name with dots and hyphens in braced form
  {
    auto result = atapp::expand_environment_expression("${deployment.environment-name}");
    CASE_EXPECT_EQ("staging", result);
  }

  // 23. k8s label-style variable with default
  {
    auto result = atapp::expand_environment_expression("${area.region:-us-west-2}");
    CASE_EXPECT_EQ("cn-east-1", result);
  }

  // 24. Bare $VAR stops at dot (POSIX: only [A-Za-z0-9_])
  {
    // $ATAPP_EXPR_DIRECT_NAME is expanded, then ".suffix" is literal text
    auto result = atapp::expand_environment_expression("$ATAPP_EXPR_DIRECT_NAME.suffix");
    CASE_EXPECT_EQ("my_app.suffix", result);
  }

  // 25. Bare $VAR stops at hyphen (POSIX-only)
  {
    auto result = atapp::expand_environment_expression("$ATAPP_EXPR_DIRECT_NAME-suffix");
    CASE_EXPECT_EQ("my_app-suffix", result);
  }

  // 26. Three-level nested default: ${A:-${B:-${C:-deep}}}
  {
    auto result = atapp::expand_environment_expression(
        "${ATAPP_EXPR_UNDEFINED_L1:-${ATAPP_EXPR_UNDEFINED_L2:-${ATAPP_EXPR_UNDEFINED_L3:-level3_default}}}");
    CASE_EXPECT_EQ("level3_default", result);
  }

  // 27. Three-level nested default, middle level defined
  {
    setenv("ATAPP_EXPR_LEVEL2", "level2_value", 1);
    auto result = atapp::expand_environment_expression(
        "${ATAPP_EXPR_UNDEFINED_L1:-${ATAPP_EXPR_LEVEL2:-${ATAPP_EXPR_UNDEFINED_L3:-level3_default}}}");
    CASE_EXPECT_EQ("level2_value", result);
    unsetenv("ATAPP_EXPR_LEVEL2");
  }

  // 28. k8s label-style variable missing, with nested default containing k8s-style name
  {
    auto result = atapp::expand_environment_expression("${missing.k8s.label:-${area.region:-fallback}}");
    CASE_EXPECT_EQ("cn-east-1", result);
  }

  // Cleanup environment variables
  unsetenv("ATAPP_EXPR_DIRECT_NAME");
  unsetenv("ATAPP_EXPR_DIRECT_HOST");
  unsetenv("ATAPP_EXPR_DIRECT_SUFFIX");
  unsetenv("ATAPP_EXPR_DIRECT_NESTED_PROD");
  unsetenv("ATAPP_EXPR_DIRECT_OUTER");
  unsetenv("ATAPP_EXPR_DIRECT_INNER");
  unsetenv("ATAPP_EXPR_DIRECT_EMPTY");
  unsetenv("app.kubernetes.io/name");
  unsetenv("deployment.environment-name");
  unsetenv("area.region");
}

// Test expression expansion via YAML config loading path
CASE_TEST(atapp_configure, expression_yaml) {
  atframework::atapp::app app;
  std::string conf_path;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_expression_test.yaml";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.expression_yaml"
                    << '\n';
    return;
  }

  // Set environment variables that the test config references
  setenv("ATAPP_EXPR_TEST_NAME", "test_app", 1);
  setenv("ATAPP_EXPR_TEST_TYPE", "gamesvr", 1);
  setenv("ATAPP_EXPR_TEST_HOST", "yaml-host.example.com", 1);
  setenv("ATAPP_EXPR_TEST_SUFFIX", "ALPHA", 1);
  setenv("ATAPP_EXPR_TEST_NESTED_ALPHA", "alpha_nested_value", 1);
  setenv("ATAPP_EXPR_TEST_LABEL_KEY", "env_label", 1);
  setenv("ATAPP_EXPR_TEST_LABEL_VALUE", "env_label_value", 1);

  const char *argv[] = {"unit-test", "-c", &conf_path[0], "--version"};
  app.init(nullptr, 4, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::protocol::atapp_configure app_cfg;
  app.parse_configures_into(app_cfg, "atapp", "", &existed_app_keys);

  // Check expression-expanded fields
  // name: "$ATAPP_EXPR_TEST_NAME" -> "test_app"
  CASE_EXPECT_EQ("test_app", app_cfg.name());
  CASE_MSG_INFO() << "name = " << app_cfg.name() << '\n';

  // type_name: "${ATAPP_EXPR_TEST_TYPE}" -> "gamesvr"
  CASE_EXPECT_EQ("gamesvr", app_cfg.type_name());
  CASE_MSG_INFO() << "type_name = " << app_cfg.type_name() << '\n';

  // hostname: "${ATAPP_EXPR_TEST_HOST:-default_host}" -> "yaml-host.example.com"
  CASE_EXPECT_EQ("yaml-host.example.com", app_cfg.hostname());
  CASE_MSG_INFO() << "hostname = " << app_cfg.hostname() << '\n';

  // identity: "${ATAPP_EXPR_TEST_MISSING_VAR:-fallback_identity}" -> "fallback_identity"
  CASE_EXPECT_EQ("fallback_identity", app_cfg.identity());
  CASE_MSG_INFO() << "identity = " << app_cfg.identity() << '\n';

  // metadata.namespace_name: "${ATAPP_EXPR_TEST_NAME:+ns_override}" -> "ns_override"
  CASE_EXPECT_EQ("ns_override", app_cfg.metadata().namespace_name());
  CASE_MSG_INFO() << "metadata.namespace_name = " << app_cfg.metadata().namespace_name() << '\n';

  // metadata.uid: "${ATAPP_EXPR_TEST_MISSING_VAR:+should_not_appear}" -> ""
  CASE_EXPECT_EQ("", app_cfg.metadata().uid());

  // metadata.service_subset: "${...:-${...:-multi_level_default}}" -> "multi_level_default" (multi-level nested)
  CASE_EXPECT_EQ("multi_level_default", app_cfg.metadata().service_subset());
  CASE_MSG_INFO() << "metadata.service_subset = " << app_cfg.metadata().service_subset() << '\n';

  // metadata.name: "${ATAPP_EXPR_TEST_NESTED_${ATAPP_EXPR_TEST_SUFFIX}}" -> "alpha_nested_value"
  CASE_EXPECT_EQ("alpha_nested_value", app_cfg.metadata().name());
  CASE_MSG_INFO() << "metadata.name = " << app_cfg.metadata().name() << '\n';

  // Check map labels
  {
    auto map_kv_expr = app_cfg.metadata().labels().find("env_label");
    CASE_EXPECT_TRUE(map_kv_expr != app_cfg.metadata().labels().end());
    if (map_kv_expr != app_cfg.metadata().labels().end()) {
      CASE_EXPECT_EQ("env_label_value", map_kv_expr->second);
      CASE_MSG_INFO() << "metadata.labels[env_label] = " << map_kv_expr->second << '\n';
    }
  }
  {
    auto map_kv_literal = app_cfg.metadata().labels().find("literal_key");
    CASE_EXPECT_TRUE(map_kv_literal != app_cfg.metadata().labels().end());
    if (map_kv_literal != app_cfg.metadata().labels().end()) {
      CASE_EXPECT_EQ("literal_value", map_kv_literal->second);
    }
  }

  WLOG_GETCAT(0)->clear_sinks();

  // Cleanup
  unsetenv("ATAPP_EXPR_TEST_NAME");
  unsetenv("ATAPP_EXPR_TEST_TYPE");
  unsetenv("ATAPP_EXPR_TEST_HOST");
  unsetenv("ATAPP_EXPR_TEST_SUFFIX");
  unsetenv("ATAPP_EXPR_TEST_NESTED_ALPHA");
  unsetenv("ATAPP_EXPR_TEST_LABEL_KEY");
  unsetenv("ATAPP_EXPR_TEST_LABEL_VALUE");
}

// Test expression expansion via .conf (INI) config loading path
CASE_TEST(atapp_configure, expression_conf) {
  atframework::atapp::app app;
  std::string conf_path;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_expression_test.conf";

  if (!atfw::util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.expression_conf"
                    << '\n';
    return;
  }

  // Set environment variables that the test config references
  setenv("ATAPP_EXPR_TEST_NAME", "test_app", 1);
  setenv("ATAPP_EXPR_TEST_TYPE", "gamesvr", 1);
  setenv("ATAPP_EXPR_TEST_HOST", "conf-host.example.com", 1);
  setenv("ATAPP_EXPR_TEST_SUFFIX", "BETA", 1);
  setenv("ATAPP_EXPR_TEST_NESTED_BETA", "beta_nested_value", 1);
  setenv("ATAPP_EXPR_TEST_LABEL_KEY", "conf_label", 1);
  setenv("ATAPP_EXPR_TEST_LABEL_VALUE", "conf_label_value", 1);

  const char *argv[] = {"unit-test", "-c", &conf_path[0], "--version"};
  app.init(nullptr, 4, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::protocol::atapp_configure app_cfg;
  app.parse_configures_into(app_cfg, "atapp", "", &existed_app_keys);

  // Check expression-expanded fields
  CASE_EXPECT_EQ("test_app", app_cfg.name());
  CASE_MSG_INFO() << "name = " << app_cfg.name() << '\n';

  CASE_EXPECT_EQ("gamesvr", app_cfg.type_name());
  CASE_MSG_INFO() << "type_name = " << app_cfg.type_name() << '\n';

  CASE_EXPECT_EQ("conf-host.example.com", app_cfg.hostname());
  CASE_MSG_INFO() << "hostname = " << app_cfg.hostname() << '\n';

  CASE_EXPECT_EQ("fallback_identity", app_cfg.identity());
  CASE_MSG_INFO() << "identity = " << app_cfg.identity() << '\n';

  // metadata.namespace_name: "${ATAPP_EXPR_TEST_NAME:+ns_override}" -> "ns_override"
  CASE_EXPECT_EQ("ns_override", app_cfg.metadata().namespace_name());

  // metadata.uid: "${ATAPP_EXPR_TEST_MISSING_VAR:+should_not_appear}" -> ""
  CASE_EXPECT_EQ("", app_cfg.metadata().uid());

  // metadata.service_subset: "${...:-${...:-multi_level_default}}" -> "multi_level_default"
  CASE_EXPECT_EQ("multi_level_default", app_cfg.metadata().service_subset());

  // metadata.name: "${ATAPP_EXPR_TEST_NESTED_${ATAPP_EXPR_TEST_SUFFIX}}" -> "beta_nested_value"
  CASE_EXPECT_EQ("beta_nested_value", app_cfg.metadata().name());
  CASE_MSG_INFO() << "metadata.name = " << app_cfg.metadata().name() << '\n';

  // Check map labels
  {
    auto map_kv_expr = app_cfg.metadata().labels().find("conf_label");
    CASE_EXPECT_TRUE(map_kv_expr != app_cfg.metadata().labels().end());
    if (map_kv_expr != app_cfg.metadata().labels().end()) {
      CASE_EXPECT_EQ("conf_label_value", map_kv_expr->second);
    }
  }
  {
    auto map_kv_literal = app_cfg.metadata().labels().find("literal_key");
    CASE_EXPECT_TRUE(map_kv_literal != app_cfg.metadata().labels().end());
    if (map_kv_literal != app_cfg.metadata().labels().end()) {
      CASE_EXPECT_EQ("literal_value", map_kv_literal->second);
    }
  }

  WLOG_GETCAT(0)->clear_sinks();

  // Cleanup
  unsetenv("ATAPP_EXPR_TEST_NAME");
  unsetenv("ATAPP_EXPR_TEST_TYPE");
  unsetenv("ATAPP_EXPR_TEST_HOST");
  unsetenv("ATAPP_EXPR_TEST_SUFFIX");
  unsetenv("ATAPP_EXPR_TEST_NESTED_BETA");
  unsetenv("ATAPP_EXPR_TEST_LABEL_KEY");
  unsetenv("ATAPP_EXPR_TEST_LABEL_VALUE");
}

CASE_TEST_EVENT_ON_EXIT(unit_test_event_on_exit_shutdown_protobuf) {
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::ShutdownProtobufLibrary();
}

CASE_TEST_EVENT_ON_EXIT(unit_test_event_on_exit_close_libuv) {
  int finish_event = 2048;
  while (0 != uv_loop_alive(uv_default_loop()) && finish_event-- > 0) {
    uv_run(uv_default_loop(), UV_RUN_NOWAIT);
  }
  uv_loop_close(uv_default_loop());
}
