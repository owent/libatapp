// Copyright 2024 atframework
// Created by owent

#include "atframe/modules/worker_pool_module.h"

#include <config/compile_optimize.h>

#include <atframe/atapp.h>

#include <oneapi/tbb/concurrent_queue.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace atapp {

class UTIL_SYMBOL_LOCAL worker_pool_module::worker {
 public:
  worker(worker_pool_module::worker_set& owner, int32_t worker_id);

  ~worker();

  worker_pool_module::worker_set& get_owner() noexcept { return *owner_; }

  static void start(std::shared_ptr<worker> self);

  inline const worker_context& get_context() const noexcept { return context_; }

 private:
  worker_pool_module::worker_set* owner_;
  worker_context context_;

  std::mutex background_job_lock_;
  std::shared_ptr<std::thread> background_job_thread_;

  ::tbb::concurrent_queue<worker_job_action_type> private_jobs;
  worker_tick_action_container_type tick_handles_;
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::worker_set {
  std::atomic<bool> closing_;
  std::atomic<int32_t> current_expect_workers;

  std::atomic<int64_t> configure_tick_interval_nanos;

  std::vector<std::shared_ptr<worker>> workers;
  ::tbb::concurrent_queue<worker_job_action_type> shared_jobs;

  std::condition_variable waker_cv_;
  std::mutex waker_lock_;
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::scaling_statistics {
  std::vector<std::shared_ptr<worker>> workers;
};

worker_pool_module::worker::worker(worker_pool_module::worker_set& owner, int32_t worker_id) : owner_(&owner) {
  context_.worker_id = worker_id;
}

worker_pool_module::worker::~worker() {
  std::shared_ptr<std::thread> background_job_thread;
  if (background_job_thread_) {
    std::lock_guard<std::mutex> lg{background_job_lock_};
    if (background_job_thread_ && background_job_thread_->joinable()) {
      background_job_thread.swap(background_job_thread_);
    }
  }

  if (background_job_thread) {
    background_job_thread->join();
  }
}

void worker_pool_module::worker::start(std::shared_ptr<worker> self) {
  if (!self) {
    return;
  }

  std::lock_guard<std::mutex> lg{self->background_job_lock_};

  if (self->background_job_thread_) {
    return;
  }

  self->background_job_thread_ = std::make_shared<std::thread>([self]() {
    // loop util end
    while (!self->get_owner().closing_.load(std::memory_order_acquire) &&
           self->get_context().worker_id < self->get_owner().current_expect_workers.load(std::memory_order_acquire)) {
      worker_job_action_type fn;
      if (self->get_owner().shared_jobs.try_pop(fn)) {
        fn(self->get_context());
      }
    }

    // exit
    {
      std::lock_guard<std::mutex> child_lg{self->background_job_lock_};
      if (self->background_job_thread_ && self->background_job_thread_->joinable() &&
          self->background_job_thread_->get_id() == std::this_thread::get_id()) {
        self->background_job_thread_->detach();
      }
    }
  });
}

LIBATAPP_MACRO_API worker_pool_module::worker_pool_module() {}

LIBATAPP_MACRO_API worker_pool_module::~worker_pool_module() { internal_cleanup(); }

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

LIBATAPP_MACRO_API const char* worker_pool_module::name() const { return "atapp: worker pool module"; }

LIBATAPP_MACRO_API int worker_pool_module::tick() {
  // TODO: tick
  return 0;
}

LIBATAPP_MACRO_API void worker_pool_module::cleanup() { internal_cleanup(); }

void worker_pool_module::internal_cleanup() {}

void worker_pool_module::apply_configure() {}

}  // namespace atapp
