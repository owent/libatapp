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
#include <vector>

#include "frame/test_macros.h"

CASE_TEST(atapp_message, send_message_remote) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_1.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  std::string conf_path_2 = conf_path_base + "/atapp_test_2.yaml";
  if (!atfw::util::file_system::is_exist(conf_path_2.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_2 << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app1;
  atframework::atapp::app app2;
  const char* args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  const char* args2[] = {"app2", "-c", conf_path_2.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));
  CASE_EXPECT_EQ(0, app2.init(nullptr, 4, args2, nullptr));

  for (int i = 0; i < 32; ++i) {
    CASE_EXPECT_GE(app1.run_noblock(), 0);
    CASE_EXPECT_GE(app2.run_noblock(), 0);
  }

  uint64_t set_sequence = 691;
  uint64_t expect_sequence = 691;
  char expect_message[] = "hello app2";
  gsl::span<const unsigned char> expect_message_span{reinterpret_cast<const unsigned char*>(expect_message),
                                                     static_cast<size_t>(strlen(expect_message))};

  int received_messge_count = 0;
  auto message_callback_fn = [expect_message, &app1, &expect_sequence, &received_messge_count](
                                 atframework::atapp::app&, const atframework::atapp::app::message_sender_t& sender,
                                 const atframework::atapp::app::message_t& msg) {
    CASE_EXPECT_EQ(app1.get_app_id(), sender.id);
    CASE_EXPECT_EQ(app1.get_app_name(), sender.name);

    CASE_EXPECT_EQ(223, msg.type);
    CASE_EXPECT_EQ(expect_sequence++, msg.message_sequence);

    auto received_message = gsl::string_view{reinterpret_cast<const char*>(msg.data.data()), msg.data.size()};
    CASE_EXPECT_EQ(expect_message, received_message);
    CASE_MSG_INFO() << "Got message: " << received_message << std::endl;

    ++received_messge_count;
    return 0;
  };

  app2.set_evt_on_forward_request(message_callback_fn);

  auto app2_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  {
    atapp::protocol::atapp_discovery app2_discovery_info;
    app2.pack(app2_discovery_info);
    app2_discovery->copy_from(app2_discovery_info, atapp::etcd_discovery_node::node_version());
    CASE_EXPECT_TRUE(app1.mutable_endpoint(app2_discovery));
  }

  {
    // Mutable app1 in app2 to get name of remote node
    auto app1_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
    atapp::protocol::atapp_discovery app1_discovery_info;
    app1.pack(app1_discovery_info);
    app1_discovery->copy_from(app1_discovery_info, atapp::etcd_discovery_node::node_version());
    CASE_EXPECT_TRUE(app2.mutable_endpoint(app1_discovery));
  }

  CASE_MSG_INFO() << "Start to send message..." << std::endl;
  auto now = atfw::util::time::time_utility::sys_now();
  auto end_time = now + std::chrono::seconds(3);

  CASE_EXPECT_EQ(0, app1.send_message(app2.get_app_id(), 223, expect_message_span, &set_sequence));
  ++set_sequence;

  CASE_EXPECT_EQ(0, app1.send_message(app2.get_app_name(), 223, expect_message_span, &set_sequence));
  ++set_sequence;

  CASE_EXPECT_EQ(2, app1.mutable_endpoint(app2_discovery)->get_pending_message_count());

  while (received_messge_count < 2 && end_time > now) {
    app1.run_noblock();
    app2.run_noblock();

    now = atfw::util::time::time_utility::sys_now();
    atfw::util::time::time_utility::update();
  }

  CASE_EXPECT_EQ(0, app1.mutable_endpoint(app2_discovery)->get_pending_message_count());

  CASE_EXPECT_EQ(0, app1.send_message(app2_discovery, 223, expect_message_span, &set_sequence));
  ++set_sequence;

  while (received_messge_count < 3 && end_time > now) {
    app1.run_noblock();
    app2.run_noblock();

    now = atfw::util::time::time_utility::sys_now();
    atfw::util::time::time_utility::update();
  }

  CASE_EXPECT_EQ(received_messge_count, 3);
}

CASE_TEST(atapp_message, send_message_loopback) {
  std::string conf_path_base;
  atfw::util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path_1 = conf_path_base + "/atapp_test_0.yaml";

  if (!atfw::util::file_system::is_exist(conf_path_1.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path_1 << " not found, skip this test" << std::endl;
    return;
  }

  atframework::atapp::app app1;
  const char* args1[] = {"app1", "-c", conf_path_1.c_str(), "start"};
  CASE_EXPECT_EQ(0, app1.init(nullptr, 4, args1, nullptr));

  for (int i = 0; i < 32; ++i) {
    CASE_EXPECT_GE(app1.run_noblock(), 0);
  }

  uint64_t set_sequence = 671;
  uint64_t expect_sequence = 671;
  char expect_message[] = "hello loopback";
  gsl::span<const unsigned char> expect_message_span{reinterpret_cast<const unsigned char*>(expect_message),
                                                     static_cast<size_t>(strlen(expect_message))};

  int received_messge_count = 0;
  app1.set_evt_on_forward_request([expect_message, &expect_sequence, &received_messge_count](
                                      atframework::atapp::app& app,
                                      const atframework::atapp::app::message_sender_t& sender,
                                      const atframework::atapp::app::message_t& msg) {
    CASE_EXPECT_EQ(app.get_app_id(), sender.id);
    CASE_EXPECT_EQ(app.get_app_name(), sender.name);

    CASE_EXPECT_EQ(321, msg.type);
    CASE_EXPECT_EQ(expect_sequence++, msg.message_sequence);

    auto received_message = gsl::string_view{reinterpret_cast<const char*>(msg.data.data()), msg.data.size()};
    CASE_EXPECT_EQ(expect_message, received_message);
    CASE_MSG_INFO() << "Got message: " << received_message << std::endl;

    ++received_messge_count;
    return 0;
  });

  auto now = atfw::util::time::time_utility::sys_now();
  auto end_time = now + std::chrono::seconds(3);

  CASE_EXPECT_EQ(0, app1.send_message(app1.get_app_id(), 321, expect_message_span, &set_sequence));
  ++set_sequence;

  CASE_EXPECT_EQ(0, app1.send_message(app1.get_app_name(), 321, expect_message_span, &set_sequence));
  ++set_sequence;

  while (received_messge_count < 2 && end_time > now) {
    app1.run_once(1, end_time - now);
    now = atfw::util::time::time_utility::sys_now();
    atfw::util::time::time_utility::update();
  }
  CASE_EXPECT_EQ(received_messge_count, 2);

  auto self_discovery = atfw::util::memory::make_strong_rc<atapp::etcd_discovery_node>();
  atapp::protocol::atapp_discovery self_discovery_info;
  app1.pack(self_discovery_info);
  self_discovery->copy_from(self_discovery_info, atapp::etcd_discovery_node::node_version());
  CASE_EXPECT_TRUE(app1.mutable_endpoint(self_discovery));

  now = atfw::util::time::time_utility::sys_now();
  end_time = now + std::chrono::seconds(3);

  CASE_EXPECT_EQ(0, app1.send_message(self_discovery, 321, expect_message_span, &set_sequence));
  ++set_sequence;

  CASE_EXPECT_EQ(0, app1.send_message(app1.get_app_name(), 321, expect_message_span, &set_sequence));
  ++set_sequence;

  while (received_messge_count < 4 && end_time > now) {
    app1.run_once(1, end_time - now);
    now = atfw::util::time::time_utility::sys_now();
    atfw::util::time::time_utility::update();
  }

  CASE_EXPECT_EQ(received_messge_count, 4);
}
