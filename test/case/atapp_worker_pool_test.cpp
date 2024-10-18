// Copyright 2022 atframework

#include <atframe/atapp.h>

#include <common/file_system.h>

#include <atframe/modules/worker_pool_module.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "frame/test_macros.h"

CASE_TEST(atapp_worker_pool, basic_spawn) {
  std::string conf_path_base;
  util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_1.yaml";

  if (!util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << std::endl;
    return;
  }

  atapp::app app;
  const char* args[] = {"app", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app.init(nullptr, 4, args, nullptr));

  std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();

  auto worker_pool_module = app.get_worker_pool_module();
  CASE_EXPECT_TRUE(!!worker_pool_module);

  if (!worker_pool_module) {
    return;
  }

  CASE_EXPECT_EQ(0, worker_pool_module->get_current_worker_count());

  std::shared_ptr<std::atomic<int32_t>> counter = std::make_shared<std::atomic<int32_t>>(0);
  CASE_EXPECT_EQ(0, worker_pool_module->spawn([counter](const atapp::worker_context&) {
    counter->fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }));

  worker_pool_module->tick(start_time + std::chrono::milliseconds(100));

  worker_pool_module->tick(start_time + std::chrono::milliseconds(200));

  worker_pool_module->tick(start_time + std::chrono::milliseconds(300));

  CASE_EXPECT_EQ(0, worker_pool_module->spawn([counter](const atapp::worker_context&) {
    counter->fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
  }));

  // Test foreach
  worker_pool_module->foreach_worker(
      [&worker_pool_module](const atapp::worker_context& context, const atapp::worker_meta& meta) -> bool {
        if (context.worker_id > worker_pool_module->get_configure_worker_except_count()) {
          CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kPendingToDestroy);
        } else if (context.worker_id > worker_pool_module->get_configure_worker_min_count()) {
          CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kDynamic);
        } else {
          CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kStable);
        }
        return true;
      });

  // Test foreach
  worker_pool_module->foreach_worker_quickly(
      [&worker_pool_module](const atapp::worker_context& context, const atapp::worker_meta& meta) -> bool {
        if (context.worker_id > worker_pool_module->get_configure_worker_except_count()) {
          CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kPendingToDestroy);
        } else if (context.worker_id > worker_pool_module->get_configure_worker_min_count()) {
          CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kDynamic);
        } else {
          CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kStable);
        }
        return true;
      });

  int32_t sleep_ms = 10000;
  while (counter->load(std::memory_order_acquire) < 2 && sleep_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sleep_ms -= 100;
  }

  CASE_EXPECT_GE(worker_pool_module->get_statistics_last_second_busy_cpu_time().count(), 2);
  CASE_EXPECT_GE(worker_pool_module->get_statistics_last_minute_busy_cpu_time().count(), 2);
  CASE_EXPECT_EQ(worker_pool_module->get_configure_worker_except_count(),
                 worker_pool_module->get_current_worker_count());
  CASE_EXPECT_GE(worker_pool_module->get_configure_worker_except_count(),
                 worker_pool_module->get_configure_worker_min_count());
  CASE_EXPECT_LE(worker_pool_module->get_configure_worker_except_count(),
                 worker_pool_module->get_configure_worker_max_count());

  worker_pool_module->tick(start_time + std::chrono::milliseconds(400));

  worker_pool_module->tick(start_time + std::chrono::milliseconds(500));

  worker_pool_module->tick(start_time + std::chrono::milliseconds(600));

  CASE_EXPECT_EQ(2, counter->load(std::memory_order_acquire));
}

// stop and cleanup
CASE_TEST(atapp_worker_pool, stop) {
  std::string conf_path_base;
  util::file_system::dirname(__FILE__, 0, conf_path_base);
  std::string conf_path = conf_path_base + "/atapp_test_one_worker.yaml";

  if (!util::file_system::is_exist(conf_path.c_str())) {
    CASE_MSG_INFO() << CASE_MSG_FCOLOR(YELLOW) << conf_path << " not found, skip this test" << std::endl;
    return;
  }

  atapp::app app;
  const char* args[] = {"app", "-c", conf_path.c_str(), "start"};
  CASE_EXPECT_EQ(0, app.init(nullptr, 4, args, nullptr));

  std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();

  auto worker_pool_module = app.get_worker_pool_module();
  CASE_EXPECT_TRUE(!!worker_pool_module);

  if (!worker_pool_module) {
    return;
  }

  CASE_EXPECT_EQ(0, worker_pool_module->get_current_worker_count());
  CASE_EXPECT_EQ(1, worker_pool_module->get_configure_worker_max_count());
  CASE_EXPECT_EQ(1, worker_pool_module->get_configure_worker_min_count());
  CASE_EXPECT_EQ(4, worker_pool_module->get_configure_worker_queue_size());
  CASE_EXPECT_EQ(10000, worker_pool_module->get_configure_tick_interval().count());

  std::this_thread::sleep_for(std::chrono::milliseconds(32));
  worker_pool_module->tick(std::chrono::system_clock::now());
  std::this_thread::sleep_for(std::chrono::milliseconds(32));
  worker_pool_module->tick(std::chrono::system_clock::now());
  std::this_thread::sleep_for(std::chrono::milliseconds(32));
  worker_pool_module->tick(std::chrono::system_clock::now());

  std::shared_ptr<std::atomic<int32_t>> counter = std::make_shared<std::atomic<int32_t>>(0);
  CASE_EXPECT_EQ(0, worker_pool_module->spawn([counter](const atapp::worker_context&) {
    counter->fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }));
  CASE_EXPECT_EQ(0, worker_pool_module->spawn([counter](const atapp::worker_context&) {
    counter->fetch_add(1, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }));

  CASE_EXPECT_EQ(1, worker_pool_module->get_configure_worker_except_count());
  worker_pool_module->stop();
  CASE_EXPECT_EQ(1, worker_pool_module->get_current_worker_count());
  CASE_EXPECT_EQ(0, worker_pool_module->get_configure_worker_except_count());
  std::this_thread::sleep_for(std::chrono::milliseconds(32));
  worker_pool_module->tick(std::chrono::system_clock::now());

  int32_t sleep_ms = 1600;
  while (worker_pool_module->get_current_worker_count() > 0 && sleep_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sleep_ms -= 100;
    start_time = std::chrono::system_clock::now();
    worker_pool_module->tick(start_time);
  }

  CASE_EXPECT_EQ(0, worker_pool_module->get_current_worker_count());
  CASE_EXPECT_GT(2, counter->load(std::memory_order_acquire));

  sleep_ms = 10000;
  while (counter->load(std::memory_order_acquire) < 2 && sleep_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sleep_ms -= 100;
    start_time = std::chrono::system_clock::now();
    worker_pool_module->tick(start_time);
  }

  CASE_EXPECT_GE(worker_pool_module->get_statistics_last_second_busy_cpu_time().count(), 0);
  CASE_EXPECT_GE(worker_pool_module->get_statistics_last_minute_busy_cpu_time().count(), 0);
  CASE_EXPECT_EQ(2, counter->load(std::memory_order_acquire));
}

// TODO: spawn with context and ignore the load balance
// TODO: scaling up
// TODO: scaling down
// TODO: rebalance pending jobs
// TODO: closing and rebalance pending jobs
