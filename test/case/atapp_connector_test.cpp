// Copyright 2022 atframework

#include <atframe/atapp.h>

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

CASE_TEST(atapp_connector, get_address_type) {
  atframework::atapp::app app;

  CASE_EXPECT_EQ(app.get_address_type("unknown://localhost"),
                 (uint32_t)atframework::atapp::app::address_type_t::EN_ACAT_NONE);

  CASE_EXPECT_TRUE(
      !!(app.get_address_type("mem://0x12345678") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
  CASE_EXPECT_TRUE(
      !!(app.get_address_type("mem://0x12345678") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

  CASE_EXPECT_TRUE(
      !!(app.get_address_type("shm://0x12345678") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
  CASE_EXPECT_TRUE(
      !(app.get_address_type("shm://0x12345678") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

  CASE_EXPECT_TRUE(
      !!(app.get_address_type("unix:///tmp/path.sock") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
  CASE_EXPECT_TRUE(!(app.get_address_type("unix:///tmp/path.sock") &
                     atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

  CASE_EXPECT_TRUE(
      (app.get_address_type("ipv4://127.0.0.1:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
  CASE_EXPECT_TRUE(!(app.get_address_type("ipv4://127.0.0.1:1234") &
                     atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));
  CASE_EXPECT_TRUE(
      !(app.get_address_type("ipv4://1.2.3.4:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));

  CASE_EXPECT_TRUE(
      (app.get_address_type("ipv6://::1:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
  CASE_EXPECT_TRUE(
      !(app.get_address_type("ipv6://::1:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));
  CASE_EXPECT_TRUE(
      !(app.get_address_type("ipv6://2000::1:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));

  CASE_EXPECT_TRUE(
      !(app.get_address_type("dns://localhost:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
  CASE_EXPECT_TRUE(
      !(app.get_address_type("dns://localhost:1234") & atframework::atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));
}
