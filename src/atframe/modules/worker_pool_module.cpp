// Copyright 2024 atframework
// Created by owent

#include "atframe/modules/worker_pool_module.h"

#include <config/compile_optimize.h>

#include <atframe/atapp.h>

#include <oneapi/tbb/concurrent_queue.h>

#include <chrono>

namespace atapp {

namespace {
struct UTIL_SYMBOL_LOCAL worker_configure_type {
  std::chrono::system_clock::duration tick_interval;

  ::tbb::concurrent_queue<worker_job_action_type> private_jobs;
};
}  // namespace

class UTIL_SYMBOL_LOCAL worker {
 private:
  worker_context context_;
  ::tbb::concurrent_queue<worker_job_action_type> private_jobs;
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::worker_set {
  std::vector<std::shared_ptr<worker>> workers;
  ::tbb::concurrent_queue<worker_job_action_type> shared_jobs;
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::scaling_statistics {
  std::vector<std::shared_ptr<worker>> workers;
};

LIBATAPP_MACRO_API worker_pool_module::worker_pool_module() {}

LIBATAPP_MACRO_API worker_pool_module::~worker_pool_module() { cleanup(); }

LIBATAPP_MACRO_API int worker_pool_module::init() {
  apply_configure();
  return 0;
}

LIBATAPP_MACRO_API int worker_pool_module::reload() {
  // TODO: configure

  if (is_actived()) {
    apply_configure();
  }
  return 0;
}

LIBATAPP_MACRO_API const char *worker_pool_module::name() const { return "atapp: worker pool module"; }

LIBATAPP_MACRO_API int worker_pool_module::tick() {
  // TODO: tick
  return 0;
}

LIBATAPP_MACRO_API void worker_pool_module::cleanup() {}

void worker_pool_module::apply_configure() {}

}  // namespace atapp
