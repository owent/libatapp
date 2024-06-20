// Copyright 2024 atframework
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <config/compile_optimize.h>

#include <cstdint>
#include <functional>

namespace atapp {

struct UTIL_SYMBOL_VISIBLE worker_context {
  int32_t worker_id;
};

using worker_job_action_type = std::function<void(const worker_context&)>;

using worker_tick_action_type = std::function<void(const worker_context&)>;

enum class worker_type : int32_t {
  kAnyWorker = -1,
  kMain = 0,
};

}  // namespace atapp
