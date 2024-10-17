// Copyright 2024 atframework
// Created by owent

#include "atframe/modules/worker_pool_module.h"

#include <config/compile_optimize.h>
#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include <atframe/atapp.h>
#include <detail/libatbus_error.h>

#include <oneapi/tbb/concurrent_queue.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#ifdef max
#  undef max
#endif

#ifdef min
#  undef min
#endif

namespace atapp {

namespace {
enum class worker_status : uint8_t {
  kCreated = 0,
  kRunning = 1,
  kSleeping = 2,
  kExited = 3,
};

struct UTIL_SYMBOL_LOCAL worker_compare_key {
  size_t pending_job_size;
  std::chrono::microseconds::rep cpu_time_last_second_busy_us;
  std::chrono::microseconds::rep cpu_time_last_minute_busy_us;
  uint32_t worker_id;
  const void* worker_ptr;

  static worker_compare_key min() noexcept {
    worker_compare_key ret;
    ret.pending_job_size = 0;
    ret.cpu_time_last_second_busy_us = 0;
    ret.cpu_time_last_minute_busy_us = 0;
    ret.worker_id = 0;
    ret.worker_ptr = nullptr;

    return ret;
  }

  static worker_compare_key max() noexcept {
    worker_compare_key ret;
    ret.pending_job_size = std::numeric_limits<size_t>::max();
    ret.cpu_time_last_second_busy_us = std::numeric_limits<std::chrono::microseconds::rep>::max();
    ret.cpu_time_last_minute_busy_us = std::numeric_limits<std::chrono::microseconds::rep>::max();
    ret.worker_id = std::numeric_limits<uint32_t>::max();
    ret.worker_ptr = reinterpret_cast<const void*>(std::numeric_limits<uintptr_t>::max());

    return ret;
  }

  friend inline bool operator<(const worker_compare_key& l, const worker_compare_key& r) noexcept {
    if (l.pending_job_size != r.pending_job_size) {
      return l.pending_job_size < r.pending_job_size;
    }

    if (l.cpu_time_last_second_busy_us != r.cpu_time_last_second_busy_us) {
      return l.cpu_time_last_second_busy_us < r.cpu_time_last_second_busy_us;
    }

    if (l.cpu_time_last_minute_busy_us != r.cpu_time_last_minute_busy_us) {
      return l.cpu_time_last_minute_busy_us < r.cpu_time_last_minute_busy_us;
    }

    if (l.worker_id != r.worker_id) {
      return l.worker_id < r.worker_id;
    }

    return l.worker_ptr < r.worker_ptr;
  }

  friend inline bool operator==(const worker_compare_key& l, const worker_compare_key& r) noexcept {
    return l.pending_job_size == r.pending_job_size &&
           l.cpu_time_last_second_busy_us == r.cpu_time_last_second_busy_us &&
           l.cpu_time_last_minute_busy_us == r.cpu_time_last_minute_busy_us && l.worker_id == r.worker_id &&
           l.worker_ptr == r.worker_ptr;
  }

  friend inline bool operator!=(const worker_compare_key& l, const worker_compare_key& r) noexcept { return !(l == r); }
};

}  // namespace

class UTIL_SYMBOL_LOCAL worker_pool_module::worker {
  UTIL_DESIGN_PATTERN_NOCOPYABLE(worker);
  UTIL_DESIGN_PATTERN_NOMOVABLE(worker);

  friend class worker_pool_module;

 public:
  worker(worker_pool_module::worker_set& owner, uint32_t worker_id);
  ~worker();

  worker_pool_module::worker_set& get_owner() noexcept { return *owner_; }

  static void start(std::shared_ptr<worker> self, std::shared_ptr<worker_set> owner);

  inline const worker_context& get_context() const noexcept { return context_; }

  void emplace(worker_job_data&& job);

  void wakeup();

  inline worker_status get_status() const noexcept {
    return static_cast<worker_status>(status_.load(std::memory_order_acquire));
  }

  std::chrono::system_clock::duration collect_scaling_up_cpu_time() noexcept {
    auto current_value = cpu_time_busy_us_.load(std::memory_order_acquire);
    auto before_value = cpu_time_collect_scaling_up_us_.exchange(current_value, std::memory_order_acq_rel);
    if UTIL_LIKELY_CONDITION (current_value > before_value) {
      return std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::microseconds(current_value - before_value));
    }

    return std::chrono::system_clock::duration::zero();
  }

  std::chrono::system_clock::duration collect_scaling_down_cpu_time() noexcept {
    auto current_value = cpu_time_busy_us_.load(std::memory_order_acquire);
    auto before_value = cpu_time_collect_scaling_down_us_.exchange(current_value, std::memory_order_acq_rel);
    if UTIL_LIKELY_CONDITION (current_value > before_value) {
      return std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::microseconds(current_value - before_value));
    }

    return std::chrono::system_clock::duration::zero();
  }

  inline size_t get_pending_job_size() const noexcept { return private_jobs.unsafe_size(); }

  inline worker_compare_key make_compare_key() const noexcept {
    worker_compare_key ret;
    ret.pending_job_size = get_pending_job_size();
    ret.cpu_time_last_second_busy_us = cpu_time_last_second_busy_us_.load(std::memory_order_acquire);
    ret.cpu_time_last_minute_busy_us = cpu_time_last_minute_busy_us_.load(std::memory_order_acquire);
    ret.worker_id = context_.worker_id;
    ret.worker_ptr = reinterpret_cast<const void*>(this);

    return ret;
  }

  inline bool is_exiting() const noexcept {
    auto status = get_status();
    if (worker_status::kExited == status) {
      return true;
    }

    if (worker_status::kCreated == status) {
      // create for a while, but not start, leak
      auto offset = std::chrono::system_clock::now().time_since_epoch() -
                    std::chrono::system_clock::duration{created_time_.load(std::memory_order_acquire)};
      if (offset < std::chrono::seconds(-30) || offset > std::chrono::seconds(30)) {
        return true;
      }
    }

    return false;
  }

 private:
  inline worker_context& get_context() noexcept { return context_; }

  void background_job_tick(std::chrono::microseconds tick_interval_us);

 private:
  worker_pool_module::worker_set* owner_;
  worker_context context_;
  std::atomic<uint8_t> status_;
  std::atomic<std::chrono::system_clock::duration::rep> created_time_;

  std::mutex background_job_lock_;
  std::shared_ptr<std::thread> background_job_thread_;

  std::mutex waker_lock_;
  std::condition_variable waker_cv_;

  ::tbb::concurrent_queue<worker_job_data> private_jobs;
  worker_tick_action_container_type tick_handles_;
  std::recursive_mutex tick_handle_lock_;

  std::atomic<std::chrono::microseconds::rep> cpu_time_busy_us_;
  std::atomic<std::chrono::microseconds::rep> cpu_time_sleep_us_;
  std::atomic<std::chrono::microseconds::rep> cpu_time_last_second_busy_us_;
  std::atomic<std::chrono::microseconds::rep> cpu_time_last_minute_busy_us_;
  time_t cpu_checkpoint_last_second_;
  time_t cpu_checkpoint_last_minute_;

  std::atomic<std::chrono::microseconds::rep> cpu_time_collect_scaling_up_us_;
  std::atomic<std::chrono::microseconds::rep> cpu_time_collect_scaling_down_us_;
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::worker_set {
  bool need_scaling_up;
  std::atomic<bool> closing;
  std::atomic<uint32_t> current_expect_workers;

  std::atomic<int64_t> configure_tick_interval_microseconds;

  std::recursive_mutex worker_lock;
  std::vector<std::shared_ptr<worker>> workers;
  std::chrono::system_clock::duration cpu_time_collect_scaling_up_us_for_removed_workers;
  std::chrono::system_clock::duration cpu_time_collect_scaling_down_us_for_removed_workers;

  ::tbb::concurrent_queue<worker_job_data> shared_jobs;

  worker_set();

  UTIL_DESIGN_PATTERN_NOCOPYABLE(worker_set);
  UTIL_DESIGN_PATTERN_NOMOVABLE(worker_set);
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::scaling_configure {
  uint32_t queue_size_limit = 20480;
  uint32_t max_workers = 4;
  uint32_t min_workers = 2;
  int64_t scaling_up_cpu_permillage = 600;
  uint32_t scaling_up_queue_size = 16;
  int32_t scaling_up_stable_window_seconds = 10;
  int64_t scaling_down_cpu_permillage = 500;
  uint32_t scaling_down_queue_size = 12;
  int32_t scaling_down_stable_window_seconds = 10;

  int32_t leak_scan_interval_seconds = 300;

  inline scaling_configure() noexcept {}
};

struct UTIL_SYMBOL_LOCAL worker_pool_module::scaling_statistics {
  std::chrono::system_clock::time_point last_scaling_up_checkpoint;
  std::chrono::system_clock::time_point last_scaling_down_checkpoint;

  std::chrono::system_clock::time_point leak_scan_checkpoint;

  inline scaling_statistics() noexcept {
    last_scaling_up_checkpoint = std::chrono::system_clock::now();
    last_scaling_down_checkpoint = last_scaling_up_checkpoint;
    leak_scan_checkpoint = last_scaling_up_checkpoint;
  }
};

worker_pool_module::worker::worker(worker_pool_module::worker_set& owner, uint32_t worker_id) : owner_(&owner) {
  context_.worker_id = worker_id;
  status_.store(static_cast<uint8_t>(worker_status::kCreated), std::memory_order_release);
  created_time_.store(std::chrono::system_clock::now().time_since_epoch().count(), std::memory_order_release);

  cpu_time_busy_us_.store(0, std::memory_order_release);
  cpu_time_sleep_us_.store(0, std::memory_order_release);
  cpu_time_last_second_busy_us_.store(0, std::memory_order_release);
  cpu_time_last_minute_busy_us_.store(0, std::memory_order_release);
  cpu_checkpoint_last_second_ = 0;
  cpu_checkpoint_last_minute_ = 0;

  cpu_time_collect_scaling_up_us_.store(0, std::memory_order_release);
  cpu_time_collect_scaling_down_us_.store(0, std::memory_order_release);
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

void worker_pool_module::worker::start(std::shared_ptr<worker> self, std::shared_ptr<worker_set> owner) {
  if (!self || !owner) {
    return;
  }

  std::lock_guard<std::mutex> lg{self->background_job_lock_};

  if (self->background_job_thread_) {
    return;
  }

  self->background_job_thread_ = std::make_shared<std::thread>([self, owner]() {
    self->status_.store(static_cast<uint8_t>(worker_status::kRunning), std::memory_order_release);

    // loop util end
    while (!owner->closing.load(std::memory_order_acquire)) {
      if (self->get_context().worker_id > owner->current_expect_workers.load(std::memory_order_acquire)) {
        // If there are tick handle, can not exit.
        std::lock_guard<std::recursive_mutex> child_lg{self->tick_handle_lock_};
        if (self->tick_handles_.empty()) {
          break;
        }
      }

      int64_t tick_interval_us = owner->configure_tick_interval_microseconds.load(std::memory_order_acquire);
      if (tick_interval_us <= 0) {
        tick_interval_us = 8000;
      }
      std::chrono::microseconds tick_interval{tick_interval_us};

      std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();
      self->background_job_tick(std::chrono::microseconds(tick_interval_us));
      std::chrono::system_clock::time_point busy_end_time = std::chrono::system_clock::now();

      // Transfer system clock to steady clock
      if (busy_end_time <= start_time) {
        busy_end_time = start_time;
      }

      auto busy_rep = std::chrono::duration_cast<std::chrono::microseconds>(busy_end_time - start_time).count();
      self->cpu_time_busy_us_.fetch_add(busy_rep, std::memory_order_release);
      if (std::chrono::system_clock::to_time_t(busy_end_time) != self->cpu_checkpoint_last_second_) {
        // update cpu time
        self->cpu_checkpoint_last_second_ = std::chrono::system_clock::to_time_t(busy_end_time);
        std::chrono::system_clock::time_point second_start =
            std::chrono::system_clock::from_time_t(self->cpu_checkpoint_last_second_);
        self->cpu_time_last_second_busy_us_.store(
            std::chrono::duration_cast<std::chrono::microseconds>(busy_end_time - second_start).count(),
            std::memory_order_release);

        if (self->cpu_checkpoint_last_minute_ > self->cpu_checkpoint_last_second_ ||
            self->cpu_checkpoint_last_minute_ + util::time::time_utility::MINITE_SECONDS <
                self->cpu_checkpoint_last_second_) {
          self->cpu_checkpoint_last_minute_ =
              self->cpu_checkpoint_last_second_ -
              self->cpu_checkpoint_last_second_ % util::time::time_utility::MINITE_SECONDS;

          std::chrono::system_clock::time_point minute_start =
              std::chrono::system_clock::from_time_t(self->cpu_checkpoint_last_minute_);
          self->cpu_time_last_minute_busy_us_.store(
              std::chrono::duration_cast<std::chrono::microseconds>(busy_end_time - minute_start).count(),
              std::memory_order_release);
        } else {
          self->cpu_time_last_minute_busy_us_.fetch_add(busy_rep, std::memory_order_release);
        }
      } else {
        self->cpu_time_last_second_busy_us_.fetch_add(busy_rep, std::memory_order_release);
        self->cpu_time_last_minute_busy_us_.fetch_add(busy_rep, std::memory_order_release);
      }

      // Maybe sleep until timeout or next event
      if (busy_end_time - start_time < tick_interval) {
        std::unique_lock<std::mutex> lk_cv(self->waker_lock_);

        self->status_.store(static_cast<uint8_t>(worker_status::kSleeping), std::memory_order_release);
        self->waker_cv_.wait_for(lk_cv, tick_interval - (busy_end_time - start_time));
        self->status_.store(static_cast<uint8_t>(worker_status::kRunning), std::memory_order_release);

        std::chrono::system_clock::time_point sleep_end_time = std::chrono::system_clock::now();
        if (sleep_end_time > busy_end_time) {
          auto sleep_rep =
              std::chrono::duration_cast<std::chrono::microseconds>(sleep_end_time - busy_end_time).count();
          self->cpu_time_sleep_us_.fetch_add(sleep_rep, std::memory_order_release);
        }
      }
    }

    // Move unfinished jobs into shared jobs
    {
      worker_job_data job_data;
      while (self->private_jobs.try_pop(job_data)) {
        owner->shared_jobs.emplace(std::move(job_data));
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

    self->status_.store(static_cast<uint8_t>(worker_status::kExited), std::memory_order_release);
  });
}

void worker_pool_module::worker::emplace(worker_job_data&& job) {
  bool need_wakeup = private_jobs.empty();
  private_jobs.emplace(std::move(job));

  if (need_wakeup) {
    wakeup();
  }
}

void worker_pool_module::worker::wakeup() { waker_cv_.notify_one(); }

void worker_pool_module::worker::background_job_tick(std::chrono::microseconds tick_interval) {
  worker_job_data job_data;

  std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();
  int32_t no_action_counter = 256;
  while (private_jobs.try_pop(job_data)) {
    if (job_data.action) {
      if (*job_data.action) {
        no_action_counter = 0;
        (*job_data.action)(get_context());
      } else {
        --no_action_counter;
      }
    }

    if (no_action_counter <= 0) {
      std::chrono::system_clock::time_point end_time = std::chrono::system_clock::now();
      if (end_time - start_time >= tick_interval) {
        break;
      }
    }
  }

  // Private tick handles
  {
    std::lock_guard<std::recursive_mutex> child_lg{tick_handle_lock_};
    for (auto& tick_handle : tick_handles_) {
      if (!tick_handle) {
        continue;
      }

      if (!tick_handle->action) {
        continue;
      }

      tick_handle->action(get_context());
    }
  }
}

worker_pool_module::worker_set::worker_set() {
  need_scaling_up = false;

  cpu_time_collect_scaling_up_us_for_removed_workers = std::chrono::system_clock::duration::zero();
  cpu_time_collect_scaling_down_us_for_removed_workers = std::chrono::system_clock::duration::zero();

  closing.store(false, std::memory_order_release);
  current_expect_workers.store(2, std::memory_order_release);
  configure_tick_interval_microseconds.store(128, std::memory_order_release);
}

LIBATAPP_MACRO_API worker_pool_module::worker_pool_module()
    : worker_set_(std::make_shared<worker_set>()),
      scaling_configure_(std::make_shared<scaling_configure>()),
      scaling_statistics_(std::make_shared<scaling_statistics>()) {}

LIBATAPP_MACRO_API worker_pool_module::~worker_pool_module() { internal_cleanup(); }

LIBATAPP_MACRO_API int worker_pool_module::init() {
  apply_configure();
  return 0;
}

LIBATAPP_MACRO_API int worker_pool_module::reload() {
  auto& cfg = get_app()->get_origin_configure().worker_pool();
  if (worker_set_) {
    int64_t tick_interval = cfg.tick_interval().nanos() / 1000 + cfg.tick_interval().seconds() * 1000000;
    if (tick_interval <= 1000) {
      tick_interval = 128000;
    }
    worker_set_->configure_tick_interval_microseconds.store(tick_interval, std::memory_order_release);
  }

  if (scaling_configure_) {
    scaling_configure_->queue_size_limit = cfg.queue_size();
    scaling_configure_->max_workers = cfg.worker_number_max();
    scaling_configure_->min_workers = cfg.worker_number_min();
    if (scaling_configure_->min_workers > scaling_configure_->max_workers) {
      scaling_configure_->min_workers = scaling_configure_->max_workers;
    }

    if (scaling_configure_->min_workers <= 0) {
      scaling_configure_->min_workers = 1;
    }
    if (scaling_configure_->min_workers > scaling_configure_->max_workers) {
      scaling_configure_->max_workers = scaling_configure_->min_workers;
    }

    scaling_configure_->scaling_up_cpu_permillage = cfg.scaling_rules().scaling_down_cpu_permillage();
    if (scaling_configure_->scaling_up_cpu_permillage <= 0) {
      scaling_configure_->scaling_up_cpu_permillage = 600;
    }
    scaling_configure_->scaling_up_queue_size = cfg.scaling_rules().scaling_up_queue_size();
    scaling_configure_->scaling_up_stable_window_seconds =
        static_cast<int32_t>(cfg.scaling_rules().scaling_up_stabilization_window().seconds());
    if (scaling_configure_->scaling_up_stable_window_seconds <= 0) {
      scaling_configure_->scaling_up_stable_window_seconds = 10;
    }

    scaling_configure_->scaling_down_cpu_permillage = cfg.scaling_rules().scaling_down_cpu_permillage();
    if (scaling_configure_->scaling_down_cpu_permillage <= 0) {
      scaling_configure_->scaling_down_cpu_permillage = 500;
    }
    scaling_configure_->scaling_down_queue_size = cfg.scaling_rules().scaling_down_queue_size();
    scaling_configure_->scaling_down_stable_window_seconds =
        static_cast<int32_t>(cfg.scaling_rules().scaling_down_stabilization_window().seconds());
    if (scaling_configure_->scaling_down_stable_window_seconds <= 0) {
      scaling_configure_->scaling_down_stable_window_seconds = 10;
    }

    if (scaling_configure_->scaling_down_cpu_permillage > scaling_configure_->scaling_up_cpu_permillage) {
      scaling_configure_->scaling_down_cpu_permillage = scaling_configure_->scaling_up_cpu_permillage;
    }

    scaling_configure_->leak_scan_interval_seconds =
        static_cast<int32_t>(cfg.scaling_rules().leak_scan_interval().seconds());
    if (scaling_configure_->leak_scan_interval_seconds <= 0) {
      scaling_configure_->leak_scan_interval_seconds = 300;
    }
  }

  if (is_actived()) {
    apply_configure();
  } else if (worker_set_) {
    worker_set_->need_scaling_up = true;
  }
  return 0;
}

LIBATAPP_MACRO_API const char* worker_pool_module::name() const { return "atapp: worker pool module"; }

LIBATAPP_MACRO_API int worker_pool_module::tick() { return tick(std::chrono::system_clock::now()); }

LIBATAPP_MACRO_API int worker_pool_module::tick(std::chrono::system_clock::time_point now) {
  if (!worker_set_) {
    return 0;
  }

  if (worker_set_->closing.load(std::memory_order_acquire)) {
    internal_reduce_workers();
    rebalance_jobs();
    do_shared_job_on_main_thread();
    return 0;
  }

  if (!scaling_configure_ || !scaling_statistics_) {
    internal_reduce_workers();
    rebalance_jobs();
    do_shared_job_on_main_thread();
    return 0;
  }

  uint32_t expect_workers = worker_set_->current_expect_workers.load(std::memory_order_acquire);
  if (expect_workers <= 0) {
    expect_workers = scaling_configure_->min_workers;
  }
  // calculate scaling up
  do {
    if (now < scaling_statistics_->last_scaling_up_checkpoint) {
      scaling_statistics_->last_scaling_up_checkpoint = now;
      break;
    }

    if (now < scaling_statistics_->last_scaling_up_checkpoint +
                  std::chrono::seconds{scaling_configure_->scaling_up_stable_window_seconds}) {
      break;
    }

    std::chrono::system_clock::duration offset = now - scaling_statistics_->last_scaling_up_checkpoint;
    scaling_statistics_->last_scaling_up_checkpoint = now;

    if (offset.count() <= 0) {
      break;
    }

    std::chrono::system_clock::duration collect_cpu_time =
        worker_set_->cpu_time_collect_scaling_up_us_for_removed_workers;
    worker_set_->cpu_time_collect_scaling_up_us_for_removed_workers = std::chrono::system_clock::duration::zero();
    size_t queue_size = 0;
    {
      std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
      for (auto& worker : worker_set_->workers) {
        if (!worker) {
          continue;
        }

        collect_cpu_time += worker->collect_scaling_up_cpu_time();
        queue_size += worker->get_pending_job_size();
      }
    }

    uint32_t scaling_up_target_count = static_cast<uint32_t>(((collect_cpu_time.count() * 1000) / offset.count()) /
                                                             scaling_configure_->scaling_up_cpu_permillage) +
                                       1;
    if (scaling_configure_->scaling_up_queue_size > 0) {
      uint32_t scaling_up_target_count_by_queue =
          static_cast<uint32_t>(queue_size / scaling_configure_->scaling_up_queue_size + 1);
      if (scaling_up_target_count_by_queue > scaling_up_target_count) {
        scaling_up_target_count = scaling_up_target_count_by_queue;
      }
    }
    if (scaling_up_target_count > scaling_configure_->max_workers) {
      scaling_up_target_count = scaling_configure_->max_workers;
    }
    if (scaling_up_target_count > expect_workers) {
      expect_workers = scaling_up_target_count;
    }
  } while (false);

  // calculate scaling down
  do {
    if (now < scaling_statistics_->last_scaling_down_checkpoint) {
      scaling_statistics_->last_scaling_down_checkpoint = now;
      break;
    }

    if (now < scaling_statistics_->last_scaling_down_checkpoint +
                  std::chrono::seconds{scaling_configure_->scaling_down_stable_window_seconds}) {
      break;
    }
    std::chrono::system_clock::duration offset = now - scaling_statistics_->last_scaling_down_checkpoint;
    scaling_statistics_->last_scaling_down_checkpoint = now;

    if (offset.count() <= 0) {
      break;
    }

    std::chrono::system_clock::duration collect_cpu_time =
        worker_set_->cpu_time_collect_scaling_down_us_for_removed_workers;
    worker_set_->cpu_time_collect_scaling_down_us_for_removed_workers = std::chrono::system_clock::duration::zero();
    size_t queue_size = 0;

    {
      std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
      for (auto& worker : worker_set_->workers) {
        if (!worker) {
          continue;
        }

        collect_cpu_time += worker->collect_scaling_down_cpu_time();
        queue_size += worker->get_pending_job_size();
      }
    }

    uint32_t scaling_down_target_count = static_cast<uint32_t>(((collect_cpu_time.count() * 1000) / offset.count()) /
                                                               scaling_configure_->scaling_down_cpu_permillage);
    if (scaling_configure_->scaling_down_queue_size > 0) {
      uint32_t scaling_down_target_count_by_queue =
          static_cast<uint32_t>(queue_size / scaling_configure_->scaling_down_queue_size);
      if (scaling_down_target_count_by_queue > scaling_down_target_count) {
        scaling_down_target_count = scaling_down_target_count_by_queue;
      }
    }

    if (scaling_down_target_count < scaling_configure_->min_workers) {
      scaling_down_target_count = scaling_configure_->min_workers;
    }
    if (scaling_down_target_count < expect_workers) {
      expect_workers = scaling_down_target_count;
    }
  } while (false);

  // do scaling up
  if (expect_workers != worker_set_->current_expect_workers.exchange(expect_workers, std::memory_order_acq_rel)) {
    worker_set_->need_scaling_up = true;
  }

  if (now >= scaling_statistics_->leak_scan_checkpoint -
                 std::chrono::seconds{scaling_configure_->leak_scan_interval_seconds} &&
      now < scaling_statistics_->leak_scan_checkpoint +
                std::chrono::seconds{scaling_configure_->leak_scan_interval_seconds}) {
    internal_reduce_workers();
  } else {
    scaling_statistics_->leak_scan_checkpoint = now;
    internal_autofix_workers();
  }

  rebalance_jobs();
  return 0;
}

LIBATAPP_MACRO_API int worker_pool_module::stop() {
  if (!worker_set_) {
    return 0;
  }

  if (false == worker_set_->closing.exchange(true, std::memory_order_acq_rel)) {
    worker_set_->current_expect_workers.store(0, std::memory_order_release);

    std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
    for (auto& worker : worker_set_->workers) {
      if (worker) {
        worker->wakeup();
      }
    }
  }

  // Can not finish when there is still any worker
  if (internal_reduce_workers()) {
    return 1;
  }

  // Wait for pending jobs to finish
  if (worker_set_->shared_jobs.unsafe_size() > 0) {
    return 1;
  }

  return 0;
}

LIBATAPP_MACRO_API void worker_pool_module::cleanup() { internal_cleanup(); }

LIBATAPP_MACRO_API int worker_pool_module::spawn(worker_job_action_type action) {
  return spawn(util::memory::make_strong_rc<worker_job_action_type>(std::move(action)));
}

LIBATAPP_MACRO_API int worker_pool_module::spawn(worker_job_action_pointer action) {
  if (!action) {
    return EN_ATBUS_ERR_PARAMS;
  }

  std::shared_ptr<worker> worker = select_worker();
  if (!worker) {
    if (!worker_set_ || worker_set_->closing.load(std::memory_order_acquire)) {
      return EN_ATAPP_ERR_WORKER_POOL_CLOSED;
    } else {
      return EN_ATAPP_ERR_WORKER_POOL_NO_AVAILABLE_WORKER;
    }
  }

  if (scaling_configure_) {
    if (worker->get_pending_job_size() >= scaling_configure_->queue_size_limit) {
      return EN_ATAPP_ERR_WORKER_POOL_BUSY;
    }
  }

  worker_job_data new_job;
  new_job.event = worker_job_event_type::kWorkerJobEventAction;
  new_job.action = action;
  worker->emplace(std::move(new_job));

  return EN_ATAPP_ERR_SUCCESS;
}

LIBATAPP_MACRO_API size_t worker_pool_module::get_current_worker_count() const noexcept {
  if (worker_set_) {
    std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
    return worker_set_->workers.size();
  }

  return 0;
}

LIBATAPP_MACRO_API size_t worker_pool_module::get_configure_worker_except_count() const noexcept {
  if (worker_set_) {
    return worker_set_->current_expect_workers.load(std::memory_order_acquire);
  }

  return 0;
}

LIBATAPP_MACRO_API size_t worker_pool_module::get_configure_worker_min_count() const noexcept {
  if (scaling_configure_) {
    return scaling_configure_->min_workers;
  }

  return 0;
}

LIBATAPP_MACRO_API size_t worker_pool_module::get_configure_worker_max_count() const noexcept {
  if (scaling_configure_) {
    return scaling_configure_->max_workers;
  }

  return 0;
}

LIBATAPP_MACRO_API size_t worker_pool_module::get_configure_worker_queue_size() const noexcept {
  if (scaling_configure_) {
    return scaling_configure_->queue_size_limit;
  }

  return 0;
}

LIBATAPP_MACRO_API std::chrono::microseconds worker_pool_module::get_configure_tick_interval() const noexcept {
  if (worker_set_) {
    return std::chrono::microseconds{worker_set_->configure_tick_interval_microseconds.load(std::memory_order_acquire)};
  }

  return std::chrono::microseconds::zero();
}

LIBATAPP_MACRO_API std::chrono::microseconds worker_pool_module::get_statistics_last_second_busy_cpu_time() {
  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  std::chrono::microseconds::rep ret = 0;
  for (auto& worker : worker_set_->workers) {
    if (!worker) {
      continue;
    }

    ret += worker->cpu_time_last_second_busy_us_.load(std::memory_order_acquire);
  }

  return std::chrono::microseconds{ret};
}

LIBATAPP_MACRO_API std::chrono::microseconds worker_pool_module::get_statistics_last_minute_busy_cpu_time() {
  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  std::chrono::microseconds::rep ret = 0;
  for (auto& worker : worker_set_->workers) {
    if (!worker) {
      continue;
    }

    ret += worker->cpu_time_last_minute_busy_us_.load(std::memory_order_acquire);
  }

  return std::chrono::microseconds{ret};
}

void worker_pool_module::do_shared_job_on_main_thread() {
  if (!worker_set_) {
    return;
  }

  int64_t tick_interval_us = worker_set_->configure_tick_interval_microseconds.load(std::memory_order_acquire);
  if (tick_interval_us <= 0) {
    tick_interval_us = 8000;
  }
  std::chrono::microseconds tick_interval{tick_interval_us};

  worker_job_data job_data;
  worker_context context;
  context.worker_id = 0;

  std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();
  int32_t no_action_counter = 256;
  while (worker_set_->shared_jobs.try_pop(job_data)) {
    if (job_data.action) {
      if (*job_data.action) {
        no_action_counter = 0;
        (*job_data.action)(context);
      } else {
        --no_action_counter;
      }
    }

    if (no_action_counter <= 0) {
      std::chrono::system_clock::time_point end_time = std::chrono::system_clock::now();
      if (end_time - start_time >= tick_interval) {
        break;
      }
    }
  }
}

void worker_pool_module::do_scaling_up() {
  if (!worker_set_) {
    return;
  }

  if (!worker_set_->need_scaling_up) {
    return;
  }
  worker_set_->need_scaling_up = false;

  uint32_t expect_workers = worker_set_->current_expect_workers.load(std::memory_order_acquire);
  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  for (size_t i = worker_set_->workers.size(); i < expect_workers; ++i) {
    worker_set_->workers.emplace_back(std::make_shared<worker>(*worker_set_, static_cast<uint32_t>(i + 1)));
    worker::start(worker_set_->workers.back(), worker_set_);
  }
}

bool worker_pool_module::internal_reduce_workers() {
  if (!worker_set_) {
    return false;
  }

  uint32_t expect_workers = 0;
  if (!worker_set_->closing.load(std::memory_order_acquire)) {
    expect_workers = worker_set_->current_expect_workers.load(std::memory_order_acquire);
  }

  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  while (worker_set_->workers.size() > expect_workers) {
    auto& last_worker = *worker_set_->workers.rbegin();
    if (last_worker) {
      if (!last_worker->is_exiting()) {
        last_worker->wakeup();
        break;
      }

      worker_set_->cpu_time_collect_scaling_up_us_for_removed_workers += last_worker->collect_scaling_up_cpu_time();
      worker_set_->cpu_time_collect_scaling_down_us_for_removed_workers += last_worker->collect_scaling_down_cpu_time();
    }

    worker_set_->workers.pop_back();
  }

  return !worker_set_->workers.empty();
}

void worker_pool_module::internal_autofix_workers() {
  if (!worker_set_) {
    return;
  }

  uint32_t expect_workers = 0;
  if (!worker_set_->closing.load(std::memory_order_acquire)) {
    expect_workers = worker_set_->current_expect_workers.load(std::memory_order_acquire);
  }

  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  bool need_autofix = false;
  for (size_t i = 0; !need_autofix && i < worker_set_->workers.size() && i < expect_workers; ++i) {
    if (!worker_set_->workers[i]) {
      continue;
    }

    if (!worker_set_->workers[i]->is_exiting()) {
      continue;
    }

    need_autofix = true;
  }

  if (!need_autofix) {
    return;
  }

  std::vector<std::shared_ptr<worker>> new_workers;
  new_workers.reserve(worker_set_->workers.size());
  for (auto& worker : worker_set_->workers) {
    if (!worker) {
      continue;
    }

    if (worker->is_exiting()) {
      worker_set_->cpu_time_collect_scaling_up_us_for_removed_workers += worker->collect_scaling_up_cpu_time();
      worker_set_->cpu_time_collect_scaling_down_us_for_removed_workers += worker->collect_scaling_down_cpu_time();
      continue;
    }

    new_workers.push_back(worker);
  }

  for (size_t i = 0; i < new_workers.size(); ++i) {
    new_workers[i]->get_context().worker_id = static_cast<uint32_t>(i + 1);
  }

  worker_set_->workers.swap(new_workers);
}

void worker_pool_module::internal_cleanup() {
  if (!worker_set_) {
    return;
  }

  worker_set_->closing.store(true, std::memory_order_release);
  {
    std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
    for (auto& worker : worker_set_->workers) {
      if (worker) {
        worker->wakeup();
      }
    }
  }

  while (internal_reduce_workers()) {
    std::this_thread::sleep_for(
        std::chrono::microseconds{worker_set_->configure_tick_interval_microseconds.load(std::memory_order_acquire)});
  }
}

void worker_pool_module::apply_configure() {
  if (scaling_statistics_) {
    scaling_statistics_->last_scaling_up_checkpoint = std::chrono::system_clock::from_time_t(0);
    scaling_statistics_->last_scaling_down_checkpoint = std::chrono::system_clock::from_time_t(0);
  }
}

std::shared_ptr<worker_pool_module::worker> worker_pool_module::select_worker() {
  do_scaling_up();

  uint32_t expect_workers = 4;
  if (worker_set_) {
    expect_workers = worker_set_->current_expect_workers.load(std::memory_order_acquire);
  }

  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  std::shared_ptr<worker_pool_module::worker> ret;
  worker_compare_key min_key = worker_compare_key::max();
  for (auto& worker : worker_set_->workers) {
    if (!worker) {
      continue;
    }

    if (worker->is_exiting()) {
      continue;
    }

    if (worker->get_context().worker_id > expect_workers) {
      break;
    }

    worker_compare_key cur_key = worker->make_compare_key();
    if (cur_key < min_key) {
      min_key = cur_key;
      ret = worker;
    }
  }

  return ret;
}

void worker_pool_module::rebalance_jobs() {
  if (!worker_set_) {
    return;
  }

  if (worker_set_->shared_jobs.empty()) {
    return;
  }

  do_scaling_up();

  // move jobs from shared_jobs to workers
  uint32_t expect_workers = worker_set_->current_expect_workers.load(std::memory_order_acquire);
  std::map<worker_compare_key, std::shared_ptr<worker>> workers;

  std::lock_guard<std::recursive_mutex> lg{worker_set_->worker_lock};
  for (auto& worker : worker_set_->workers) {
    if (!worker) {
      continue;
    }

    if (worker->is_exiting()) {
      continue;
    }

    if (worker->get_context().worker_id > expect_workers) {
      break;
    }

    workers[worker->make_compare_key()] = worker;
  }

  if (workers.empty()) {
    return;
  }

  worker_job_data job_data;
  while (worker_set_->shared_jobs.try_pop(job_data)) {
    auto worker = workers.begin()->second;
    worker->emplace(std::move(job_data));
    workers.erase(workers.begin());

    // Reinsert and change the order
    workers[worker->make_compare_key()] = worker;
  }
}

}  // namespace atapp
