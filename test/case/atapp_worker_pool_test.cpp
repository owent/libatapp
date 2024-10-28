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
  atapp::worker_context selected_worker;
  atapp::worker_context real_worker;

  CASE_EXPECT_EQ(0, worker_pool_module->spawn(
                        [counter, &real_worker](const atapp::worker_context& ctx) {
                          counter->fetch_add(1, std::memory_order_release);
                          real_worker = ctx;
                          std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        },
                        &selected_worker));

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
  CASE_EXPECT_EQ(selected_worker.worker_id, real_worker.worker_id);
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
  CASE_EXPECT_EQ(10000, worker_pool_module->get_configure_tick_min_interval().count());
  CASE_EXPECT_EQ(1000000, worker_pool_module->get_configure_tick_max_interval().count());

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

CASE_TEST(atapp_worker_pool, foreach_stable_workers) {
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

  auto worker_pool_module = app.get_worker_pool_module();
  CASE_EXPECT_TRUE(!!worker_pool_module);

  if (!worker_pool_module) {
    return;
  }

  CASE_EXPECT_EQ(0, worker_pool_module->get_current_worker_count());

  auto min_count = worker_pool_module->get_configure_worker_min_count();
  size_t foreach_counter = 0;
  // Test foreach
  worker_pool_module->foreach_worker([&worker_pool_module, &foreach_counter, min_count](
                                         const atapp::worker_context& context, const atapp::worker_meta& meta) -> bool {
    CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kStable);
    CASE_EXPECT_LE(context.worker_id, min_count);

    ++foreach_counter;
    return true;
  });
  CASE_EXPECT_EQ(min_count, foreach_counter);

  // Test foreach
  worker_pool_module->foreach_worker_quickly(
      [&worker_pool_module, &foreach_counter, min_count](const atapp::worker_context& context,
                                                         const atapp::worker_meta& meta) -> bool {
        CASE_EXPECT_TRUE(meta.scaling_mode == atapp::worker_scaling_mode::kStable);
        CASE_EXPECT_LE(context.worker_id, min_count);

        ++foreach_counter;
        return true;
      });

  CASE_EXPECT_EQ(min_count + min_count, foreach_counter);
}

// TODO: spawn with context and ignore the load balance
// TODO: scaling up
// TODO: scaling down
// TODO: rebalance pending jobs
// TODO: closing and rebalance pending jobs

// basic tick
CASE_TEST(atapp_worker_pool, basic_tick) {
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

  atapp::worker_context select_context;
  worker_pool_module->foreach_worker(
      [&select_context](const atapp::worker_context& context, const atapp::worker_meta&) -> bool {
        select_context = context;
        return false;
      });

  std::atomic<size_t> tick_times{0};
  auto old_tick_interval = worker_pool_module->get_tick_interval(select_context);
  auto handle = worker_pool_module->add_tick_callback(
      [&tick_times](const atapp::worker_context& /*context*/) {
        ++tick_times;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 0;
      },
      select_context);

  CASE_EXPECT_TRUE(!!handle);
  for (size_t i = 0; i < 5; ++i) {
    worker_pool_module->tick(std::chrono::system_clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CASE_MSG_INFO() << "Tick interval: " << worker_pool_module->get_tick_interval(select_context).count() << "ms"
                    << std::endl;
  }
  CASE_EXPECT_GT(tick_times.load(), 3);
  CASE_EXPECT_LT(tick_times.load(), 8);
  auto new_tick_interval = worker_pool_module->get_tick_interval(select_context);

  // Change tick interval
  CASE_EXPECT_NE(old_tick_interval.count(), new_tick_interval.count());
  CASE_EXPECT_TRUE(worker_pool_module->reset_tick_interval(select_context, std::chrono::milliseconds(0)));
  auto reset_tick_interval = worker_pool_module->get_tick_interval(select_context);
  CASE_EXPECT_LT(reset_tick_interval.count(), new_tick_interval.count());
  CASE_EXPECT_GE(reset_tick_interval.count(), worker_pool_module->get_configure_tick_min_interval().count());

  // Remove
  CASE_EXPECT_TRUE(worker_pool_module->remove_tick_callback(handle));
  size_t tick_times_before = tick_times.load();
  for (size_t i = 0; i < 6; ++i) {
    worker_pool_module->tick(std::chrono::system_clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker_pool_module->get_current_worker_count() == 0) {
      break;
    }
  }

  size_t tick_times_after = tick_times.load();
  CASE_EXPECT_LE(tick_times_after, tick_times_before + 1);
}

// TODO: busy tick and descrease tick interval
// TODO: free tick and increase tick interval

// stop tick
CASE_TEST(atapp_worker_pool, stop_tick) {
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

  atapp::worker_context select_context;
  worker_pool_module->foreach_worker(
      [&select_context](const atapp::worker_context& context, const atapp::worker_meta&) -> bool {
        select_context = context;
        return false;
      });

  std::atomic<size_t> tick_times{0};
  auto old_tick_interval = worker_pool_module->get_tick_interval(select_context);
  auto handle = worker_pool_module->add_tick_callback(
      [&tick_times](const atapp::worker_context& /*context*/) {
        ++tick_times;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 0;
      },
      select_context);

  CASE_EXPECT_TRUE(!!handle);
  for (size_t i = 0; i < 5; ++i) {
    worker_pool_module->tick(std::chrono::system_clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  auto new_tick_interval = worker_pool_module->get_tick_interval(select_context);
  CASE_EXPECT_GT(tick_times.load(), 3);
  CASE_EXPECT_LT(tick_times.load(), 8);

  // Stop
  CASE_EXPECT_NE(old_tick_interval.count(), new_tick_interval.count());
  size_t tick_times_before = tick_times.load();
  worker_pool_module->stop();

  for (size_t i = 0; i < 5; ++i) {
    worker_pool_module->tick(std::chrono::system_clock::now());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (worker_pool_module->get_current_worker_count() == 0) {
      break;
    }
  }

  size_t tick_times_after = tick_times.load();
  CASE_EXPECT_LT(tick_times_after, tick_times_before + 2);
}
