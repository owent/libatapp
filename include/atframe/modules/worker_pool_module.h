// Copyright 2024 atframework
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <nostd/function_ref.h>

#include <atframe/atapp_conf.h>
#include <atframe/atapp_module_impl.h>
#include <atframe/modules/worker_context.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

LIBATAPP_MACRO_NAMESPACE_BEGIN

class worker_pool_module : public ::atframework::atapp::module_impl {
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

  // thread-safe
  LIBATAPP_MACRO_API int spawn(worker_job_action_type action, worker_context* selected_context = nullptr);

  // thread-safe
  LIBATAPP_MACRO_API int spawn(worker_job_action_pointer action, worker_context* selected_context = nullptr);

  // thread-safe
  LIBATAPP_MACRO_API int spawn(worker_job_action_type action, const worker_context& context);

  // thread-safe
  LIBATAPP_MACRO_API int spawn(worker_job_action_pointer action, const worker_context& context);

  // thread-safe
  LIBATAPP_MACRO_API worker_tick_action_handle_type add_tick_callback(worker_tick_action_type action,
                                                                      const worker_context& context);

  // thread-safe
  LIBATAPP_MACRO_API bool remove_tick_callback(worker_tick_action_handle_type& handle);

  // thread-safe
  LIBATAPP_MACRO_API bool reset_tick_interval(const worker_context& context,
                                              std::chrono::microseconds new_tick_interval);

  // thread-safe
  LIBATAPP_MACRO_API std::chrono::microseconds get_tick_interval(const worker_context& context) const;

  // thread-safe
  LIBATAPP_MACRO_API size_t get_current_worker_count() const noexcept;

  // thread-safe
  LIBATAPP_MACRO_API void foreach_worker_quickly(
      ::atfw::util::nostd::function_ref<bool(const worker_context&, const worker_meta&)> fn) const;

  // thread-safe
  LIBATAPP_MACRO_API void foreach_worker(
      ::atfw::util::nostd::function_ref<bool(const worker_context&, const worker_meta&)> fn);

  // thread-safe, lockless
  LIBATAPP_MACRO_API size_t get_configure_worker_except_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_min_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_max_count() const noexcept;

  LIBATAPP_MACRO_API size_t get_configure_worker_queue_size() const noexcept;

  // thread-safe, lockless
  LIBATAPP_MACRO_API std::chrono::microseconds get_configure_tick_min_interval() const noexcept;

  // thread-safe, lockless
  LIBATAPP_MACRO_API std::chrono::microseconds get_configure_tick_max_interval() const noexcept;

  // thread-safe
  LIBATAPP_MACRO_API std::chrono::microseconds get_statistics_last_second_busy_cpu_time();

  // thread-safe
  LIBATAPP_MACRO_API std::chrono::microseconds get_statistics_last_minute_busy_cpu_time();

  LIBATAPP_MACRO_API static bool is_valid(const worker_context& context) noexcept;

 private:
  void do_shared_job_on_main_thread();
  void do_scaling_up();
  bool internal_reduce_workers();
  void internal_autofix_workers();
  void internal_cleanup();
  void apply_configure();

  int select_worker(const worker_context& context, std::shared_ptr<worker>& output);
  std::shared_ptr<worker> select_worker();
  void rebalance_jobs();

 private:
  std::shared_ptr<worker_set> worker_set_;
  std::shared_ptr<scaling_configure> scaling_configure_;
  std::shared_ptr<scaling_statistics> scaling_statistics_;
};
LIBATAPP_MACRO_NAMESPACE_END
