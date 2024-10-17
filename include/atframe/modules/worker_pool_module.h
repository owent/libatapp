// Copyright 2024 atframework
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <atframe/atapp_conf.h>
#include <atframe/atapp_module_impl.h>
#include <atframe/modules/worker_context.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace atapp {

class worker_pool_module : public ::atapp::module_impl {
 public:
  struct worker_set;
  struct scaling_configure;
  struct scaling_statistics;

 public:
  LIBATAPP_MACRO_API worker_pool_module();
  LIBATAPP_MACRO_API virtual ~worker_pool_module();

 private:
  class worker;

 public:
  LIBATAPP_MACRO_API int reload() override;

  LIBATAPP_MACRO_API int init() override;

  LIBATAPP_MACRO_API const char* name() const override;

  LIBATAPP_MACRO_API int tick() override;

  LIBATAPP_MACRO_API int tick(std::chrono::system_clock::time_point now);

  template <class ClockType, class DurationType>
  UTIL_FORCEINLINE int tick(std::chrono::time_point<ClockType, DurationType> now) {
    return tick(std::chrono::time_point_cast<std::chrono::system_clock::time_point>(now));
  }

  LIBATAPP_MACRO_API int stop() override;

  LIBATAPP_MACRO_API void cleanup() override;

  LIBATAPP_MACRO_API int spawn(worker_job_action_type action);

  LIBATAPP_MACRO_API int spawn(worker_job_action_pointer action);

  LIBATAPP_MACRO_API size_t get_current_worker_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_except_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_min_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_max_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_queue_size() const noexcept;

  LIBATAPP_MACRO_API std::chrono::microseconds get_configure_tick_interval() const noexcept;

  LIBATAPP_MACRO_API std::chrono::microseconds get_statistics_last_second_busy_cpu_time();

  LIBATAPP_MACRO_API std::chrono::microseconds get_statistics_last_minute_busy_cpu_time();

 private:
  void do_shared_job_on_main_thread();
  void do_scaling_up();
  bool internal_reduce_workers();
  void internal_autofix_workers();
  void internal_cleanup();
  void apply_configure();

  std::shared_ptr<worker> select_worker();
  void rebalance_jobs();

 private:
  std::shared_ptr<worker_set> worker_set_;
  std::shared_ptr<scaling_configure> scaling_configure_;
  std::shared_ptr<scaling_statistics> scaling_statistics_;
};
}  // namespace atapp
