// Copyright 2024 atframework
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <config/compile_optimize.h>

#include <memory/rc_ptr.h>

#include <cstdint>
#include <functional>
#include <list>

namespace atapp {

struct UTIL_SYMBOL_VISIBLE worker_context {
  uint32_t worker_id;
};

enum class worker_job_event_type : uint32_t {
  kWorkerJobEventAction = 0,
};

enum class worker_tick_handle_type : uint32_t {
  kWorkerTickHandleAny = 0,
  kWorkerTickHandleSpecify = 1,
};

enum class worker_scaling_mode : uint8_t {
  kStable = 0,  // Under minimal count
  kDynamic = 1, // Between minimal and maximal count
  kPendingToDestroy = 2, // Pending to destroy
};

struct UTIL_SYMBOL_VISIBLE worker_meta {
  worker_scaling_mode scaling_mode;
};

using worker_job_action_type = std::function<void(const worker_context&)>;

using worker_job_action_pointer = ::util::memory::strong_rc_ptr<worker_job_action_type>;

struct UTIL_SYMBOL_VISIBLE worker_job_data {
  worker_job_event_type event = worker_job_event_type::kWorkerJobEventAction;
  worker_job_action_pointer action;

  inline worker_job_data() noexcept {}
};

using worker_tick_action_type = std::function<void(const worker_context&)>;

struct UTIL_SYMBOL_VISIBLE worker_tick_handle_data {
  worker_tick_handle_type type;
  worker_tick_action_type action;
};

using worker_tick_action_container_type = std::list<::util::memory::strong_rc_ptr<worker_tick_handle_data>>;

using worker_tick_action_handle_type = worker_tick_action_container_type::iterator;

enum class worker_type : int32_t {
  kAnyWorker = -1,
  kMain = 0,
};

}  // namespace atapp
