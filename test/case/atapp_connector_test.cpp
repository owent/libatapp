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

#include "frame/test_macros.h"


CASE_TEST(atapp_connector, get_address_type) {
    atapp::app app;

    CASE_EXPECT_EQ(app.get_address_type("unknown://localhost"), (uint32_t)atapp::app::address_type_t::EN_ACAT_NONE);

    CASE_EXPECT_TRUE(!!(app.get_address_type("mem://0x12345678") & atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
    CASE_EXPECT_TRUE(!!(app.get_address_type("mem://0x12345678") & atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

    CASE_EXPECT_TRUE(!!(app.get_address_type("shm://0x12345678") & atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
    CASE_EXPECT_TRUE(!(app.get_address_type("shm://0x12345678") & atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

    CASE_EXPECT_TRUE(!!(app.get_address_type("unix:///tmp/path.sock") & atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
    CASE_EXPECT_TRUE(!(app.get_address_type("unix:///tmp/path.sock") & atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

    CASE_EXPECT_TRUE(!(app.get_address_type("ipv4://127.0.0.1:1234") & atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
    CASE_EXPECT_TRUE(!(app.get_address_type("ipv4://127.0.0.1:1234") & atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

    CASE_EXPECT_TRUE(!(app.get_address_type("ipv6://::1:1234") & atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
    CASE_EXPECT_TRUE(!(app.get_address_type("ipv6://::1:1234") & atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));

    CASE_EXPECT_TRUE(!(app.get_address_type("dns://localhost:1234") & atapp::app::address_type_t::EN_ACAT_LOCAL_HOST));
    CASE_EXPECT_TRUE(!(app.get_address_type("dns://localhost:1234") & atapp::app::address_type_t::EN_ACAT_LOCAL_PROCESS));
}

