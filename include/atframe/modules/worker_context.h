// Copyright 2024 atframework
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <config/compile_optimize.h>

#include <cstdint>
#include <functional>
#include <list>

namespace atapp {

struct UTIL_SYMBOL_VISIBLE worker_context {
  int32_t worker_id;
};

using worker_job_action_type = std::function<void(const worker_context&)>;

using worker_tick_action_type = std::function<void(const worker_context&)>;

using worker_tick_action_container_type = std::list<worker_tick_action_type>;

using worker_tick_action_handle_type = worker_tick_action_container_type::iterator;

enum class worker_type : int32_t {
  kAnyWorker = -1,
  kMain = 0,
};

}  // namespace atapp
