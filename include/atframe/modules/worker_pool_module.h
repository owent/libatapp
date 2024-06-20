// Copyright 2024 atframework
// Created by owent

#pragma once

#include <config/atframe_utils_build_feature.h>

#include <atframe/atapp_conf.h>
#include <atframe/atapp_module_impl.h>
#include <atframe/modules/worker_context.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace atapp {

class worker_pool_module : public ::atapp::module_impl {
 public:
  LIBATAPP_MACRO_API worker_pool_module();
  LIBATAPP_MACRO_API virtual ~worker_pool_module();

 private:
  class worker;

  struct worker_set;
  struct scaling_statistics;

 public:
  LIBATAPP_MACRO_API int reload() override;

  LIBATAPP_MACRO_API int init() override;

  LIBATAPP_MACRO_API const char* name() const override;

  LIBATAPP_MACRO_API int tick() override;

  LIBATAPP_MACRO_API void cleanup() override;

 private:
  void apply_configure();

 private:
  std::shared_ptr<worker_set> worker_set_;
  std::shared_ptr<scaling_statistics> scaling_statistics_;
};
}  // namespace atapp
