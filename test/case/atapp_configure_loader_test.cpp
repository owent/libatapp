#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <vector>

#include <atframe/atapp.h>

#include <common/file_system.h>

#include "frame/test_macros.h"

static void check_configure(atapp::app &app, atapp::protocol::atapp_etcd sub_cfg) {
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

  CASE_EXPECT_EQ(0, app.get_origin_configure().timer().tick_interval().seconds());
  CASE_EXPECT_EQ(32000000, app.get_origin_configure().timer().tick_interval().nanos());

  CASE_EXPECT_EQ(3, app.get_origin_configure().etcd().hosts_size());
  CASE_EXPECT_EQ("http://127.0.0.1:2375", app.get_origin_configure().etcd().hosts(0));
  CASE_EXPECT_EQ("http://127.0.0.1:2376", app.get_origin_configure().etcd().hosts(1));
  CASE_EXPECT_EQ("http://127.0.0.1:2377", app.get_origin_configure().etcd().hosts(2));
  CASE_EXPECT_EQ("/atapp/services/astf4g/", app.get_origin_configure().etcd().path());
  CASE_EXPECT_EQ("", app.get_origin_configure().etcd().authorization());
  CASE_EXPECT_EQ(0, app.get_origin_configure().etcd().init().tick_interval().seconds());
  CASE_EXPECT_EQ(16000000, app.get_origin_configure().etcd().init().tick_interval().nanos());

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
  app.init(NULL, 4, argv);
  app.reload();

  atapp::protocol::atapp_etcd sub_cfg;
  app.parse_configures_into(sub_cfg, "atapp.etcd");
  check_configure(app, sub_cfg);

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
  app.init(NULL, 4, argv);
  app.reload();

  atapp::protocol::atapp_etcd sub_cfg;
  app.parse_configures_into(sub_cfg, "atapp.etcd");
  check_configure(app, sub_cfg);

  WLOG_GETCAT(0)->clear_sinks();
  WLOG_GETCAT(1)->clear_sinks();
}
