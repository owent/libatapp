// Copyright 2022 atframework

#include <atframe/atapp.h>
#include <atframe/atapp_module_impl.h>

#include <common/file_system.h>
#include <time/time_utility.h>

#include <chrono>
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

#include "frame/test_macros.h"

class atapp_setup_test_timeout_module : public ::atapp::module_impl {
 public:
  int setup(::atapp::app_conf& conf) override {
    conf.origin.mutable_timer()->mutable_initialize_timeout()->set_seconds(1);
    return 0;
  }

  const char* name() const override { return "atapp_setup_test_timeout_module"; }

  int init() override {
    while (!get_app()->check_flag(::atapp::app::flag_t::TIMEOUT)) {
      get_app()->run_once(0, std::chrono::seconds{1});
    }

    // Success here should be redirect into EN_ATAPP_ERR_OPERATION_TIMEOUT by atapp::app
    return 0;
  }
};

CASE_TEST(atapp_setup, timeout) {
  std::string conf_path_base;
  util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_0.yaml";

  if (!util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  atapp::app app1;
  const char* args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};

  app1.add_module(std::make_shared<atapp_setup_test_timeout_module>());

  util::time::time_utility::update();
  auto before = util::time::time_utility::sys_now();
  CASE_EXPECT_EQ(atapp::EN_ATAPP_ERR_OPERATION_TIMEOUT, app1.init(nullptr, 4, args1, nullptr));
  util::time::time_utility::update();
  auto after = util::time::time_utility::sys_now();
  CASE_EXPECT_LE(std::chrono::duration_cast<std::chrono::seconds>(after - before).count(), 3);
}
