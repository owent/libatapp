// Copyright 2026 atframework
//
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <config/compile_optimize.h>

#include <memory/rc_ptr.h>

#include <cstdint>
#include <functional>
#include <memory>

LIBATAPP_MACRO_NAMESPACE_BEGIN

struct UTIL_SYMBOL_VISIBLE worker_context {
  // worker id 指示当前是第几个worker，0表示主线程，1表示第一个工作线程，依次类推。
  // worker id 可能被复用或转移工作线程，但同时每个 worker id 指向唯一一个线程
  uint32_t worker_id = 0;

  // worker_unique_id 指示当前worker的唯一标识，不会随着线程转移而变化
  // 注意: 在foreach接口中，如果对于stable的worker尚未分配完成，这个值可能传0
  uint64_t worker_unique_id = 0;

  inline worker_context() noexcept : worker_id(0), worker_unique_id(0) {}
  explicit inline worker_context(uint32_t id, uint64_t unique_id) noexcept
      : worker_id(id), worker_unique_id(unique_id) {}
};

enum class worker_job_event_type : uint32_t {
  kWorkerJobEventAction = 0,
};

enum class worker_tick_handle_type : uint32_t {
  kWorkerTickHandleAny = 0,
  kWorkerTickHandleSpecify = 1,
};

enum class worker_scaling_mode : uint8_t {
  kStable = 0,            // Under minimal count
  kDynamic = 1,           // Between minimal and maximal count
  kPendingToDestroy = 2,  // Pending to destroy
};

struct UTIL_SYMBOL_VISIBLE worker_meta {
  worker_scaling_mode scaling_mode;
};

using worker_job_action_type = std::function<void(const worker_context&)>;

using worker_event_callback_type = std::function<void(const worker_context&)>;

struct UTIL_SYMBOL_VISIBLE worker_event_callback_handle_data;
using worker_event_callback_handle_type = std::shared_ptr<worker_event_callback_handle_data>;

using worker_job_action_pointer = ::atfw::util::memory::strong_rc_ptr<worker_job_action_type>;

struct UTIL_SYMBOL_VISIBLE worker_job_data {
  worker_job_event_type event = worker_job_event_type::kWorkerJobEventAction;
  worker_job_action_pointer action;

  inline worker_job_data() noexcept {}
};

using worker_tick_action_type = std::function<void(const worker_context&)>;

struct UTIL_SYMBOL_VISIBLE worker_tick_handle_data {
  worker_tick_handle_type type;
  worker_tick_action_type action;

  inline worker_tick_handle_data(worker_tick_handle_type input_type, worker_tick_action_type&& input_action) noexcept
      : type(input_type), action(std::move(input_action)) {}
};

using worker_tick_action_pointer = ::atfw::util::memory::strong_rc_ptr<worker_tick_handle_data>;

struct UTIL_SYMBOL_VISIBLE worker_tick_action_handle_data;
using worker_tick_action_handle_type = std::shared_ptr<worker_tick_action_handle_data>;

enum class worker_type : int32_t {
  kAnyWorker = -1,
  kMain = 0,
};

LIBATAPP_MACRO_NAMESPACE_END
