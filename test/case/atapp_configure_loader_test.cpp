// Copyright 2022 atframework

#include <atframe/atapp.h>

#include <common/file_system.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include "frame/test_macros.h"

#if defined(_WIN32)
inline int setenv(const char *name, const char *value, int) { return _putenv_s(name, value); }
inline int unsetenv(const char *name) { return setenv(name, "", 1); }
#endif

static void check_origin_configure(atapp::app &app, atapp::protocol::atapp_etcd sub_cfg,
                                   const atapp::configure_key_set &existed_keys) {
  CASE_EXPECT_EQ(app.get_id(), 0x1234);
  CASE_EXPECT_EQ(app.get_origin_configure().id_mask(), "8.8.8.8");
  CASE_EXPECT_EQ(app.convert_app_id_by_string("1.2.3.4"), 0x01020304);

  CASE_EXPECT_EQ(1, app.get_origin_configure().bus().listen_size());
  CASE_EXPECT_EQ("ipv6://:::21437", app.get_origin_configure().bus().listen(0));

  CASE_EXPECT_EQ(30, app.get_origin_configure().bus().first_idle_timeout().seconds());
  CASE_EXPECT_EQ(60, app.get_origin_configure().bus().ping_interval().seconds());
  CASE_EXPECT_EQ(3, app.get_origin_configure().bus().retry_interval().seconds());
  CASE_EXPECT_EQ(3, app.get_origin_configure().bus().fault_tolerant());
  CASE_EXPECT_EQ(262144, app.get_origin_configure().bus().msg_size());
  CASE_EXPECT_EQ(8388608, app.get_origin_configure().bus().recv_buffer_size());
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
  CASE_EXPECT_FALSE(app.get_origin_configure().etcd().watcher().by_id());
  CASE_EXPECT_TRUE(app.get_origin_configure().etcd().watcher().by_name());

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
  CASE_EXPECT_FALSE(sub_cfg.watcher().by_id());
  CASE_EXPECT_TRUE(sub_cfg.watcher().by_name());

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

  // Check app keys
  std::string keys_path;
  util::file_system::dirname(__FILE__, 0, keys_path);
  keys_path += "/atapp_configure_loader_test_app_keys.txt";

  if (!util::file_system::is_exist(keys_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << keys_path << " not found, skip checking etcd keys" << std::endl;
    return;
  }

  std::fstream keys_ss(keys_path.c_str(), std::ios::in);
  std::string key_line;
  size_t keys_size = 0;
  while (std::getline(keys_ss, key_line)) {
    auto trimed_line = util::string::trim(key_line.c_str(), key_line.size());
    if (trimed_line.second == 0) {
      continue;
    }

    ++keys_size;
    bool check_exists = existed_keys.end() != existed_keys.find(std::string(trimed_line.first, trimed_line.second));
    if (!check_exists) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << std::string(trimed_line.first, trimed_line.second) << " not found"
                      << std::endl;
    }
    CASE_EXPECT_TRUE(check_exists);
  }
  CASE_EXPECT_EQ(keys_size, existed_keys.size());
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
  util::file_system::dirname(__FILE__, 0, keys_path);
  keys_path += "/atapp_configure_loader_test_log_keys.txt";

  if (!util::file_system::is_exist(keys_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << keys_path << " not found, skip checking etcd keys" << std::endl;
    return;
  }

  std::fstream keys_ss(keys_path.c_str(), std::ios::in);
  std::string key_line;
  size_t keys_size = 0;
  while (std::getline(keys_ss, key_line)) {
    auto trimed_line = util::string::trim(key_line.c_str(), key_line.size());
    if (trimed_line.second == 0) {
      continue;
    }

    ++keys_size;
    bool check_exists = existed_keys.end() != existed_keys.find(std::string(trimed_line.first, trimed_line.second));
    if (!check_exists) {
      CASE_MSG_INFO() << CASE_MSG_FCOLOR(RED) << std::string(trimed_line.first, trimed_line.second) << " not found"
                      << std::endl;
    }
    CASE_EXPECT_TRUE(check_exists);
  }
  CASE_EXPECT_EQ(keys_size, existed_keys.size());
}

CASE_TEST(atapp_configure, load_yaml) {
  atapp::app app;
  std::string conf_path;
  util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_loader_test.yaml";

  if (!util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.load_yaml"
                    << std::endl;
    return;
  }

  const char *argv[] = {"unit-test", "-c", &conf_path[0], "--version"};
  app.init(nullptr, 4, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::configure_key_set existed_log_keys;
  atapp::protocol::atapp_etcd sub_cfg;
  app.parse_configures_into(sub_cfg, "atapp.etcd", "", &existed_app_keys);
  check_origin_configure(app, sub_cfg, existed_app_keys);

  atapp::protocol::atapp_log app_log;
  app.parse_log_configures_into(app_log, std::vector<gsl::string_view>{"atapp", "log"}, "", &existed_log_keys);
  check_log_configure(app_log, existed_log_keys);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();
}

CASE_TEST(atapp_configure, load_conf) {
  atapp::app app;
  std::string conf_path;
  util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_loader_test.conf";

  if (!util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.load_conf"
                    << std::endl;
    return;
  }

  const char *argv[] = {"unit-test", "-c", &conf_path[0], "--version"};
  app.init(nullptr, 4, argv);
  app.reload();

  atapp::configure_key_set existed_app_keys;
  atapp::configure_key_set existed_log_keys;
  atapp::protocol::atapp_etcd sub_cfg;
  app.parse_configures_into(sub_cfg, "atapp.etcd", "", &existed_app_keys);
  check_origin_configure(app, sub_cfg, existed_app_keys);

  atapp::protocol::atapp_log app_log;
  app.parse_log_configures_into(app_log, std::vector<gsl::string_view>{"atapp", "log"}, "", &existed_log_keys);
  check_log_configure(app_log, existed_log_keys);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();
}

CASE_TEST(atapp_configure, load_environment) {
  atapp::app app;
  std::string conf_path;
  util::file_system::dirname(__FILE__, 0, conf_path);
  conf_path += "/atapp_configure_loader_test.env.txt";

  if (!util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip atapp_configure.load_environment"
                    << std::endl;
    return;
  }

  std::fstream fs(conf_path.c_str(), std::ios::in);
  std::string line;
  std::unordered_set<std::string> env_vars;
  while (std::getline(fs, line)) {
    auto trimed_line = util::string::trim(line.c_str(), line.size());
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
  atapp::configure_key_set existed_log_keys;
  atapp::protocol::atapp_etcd sub_cfg;
  app.parse_configures_into(sub_cfg, "atapp.etcd", "ATAPP_ETCD", &existed_app_keys);
  check_origin_configure(app, sub_cfg, existed_app_keys);

  atapp::protocol::atapp_log app_log;
  app.parse_log_configures_into(app_log, std::vector<gsl::string_view>{"atapp", "log"}, "ATAPP_LOG", &existed_log_keys);
  check_log_configure(app_log, existed_log_keys);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();

  for (auto &env_var : env_vars) {
    unsetenv(env_var.c_str());
  }
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
