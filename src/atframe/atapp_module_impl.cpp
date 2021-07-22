// Copyright 2021 atframework
// Created by owent

#include <signal.h>
#include <iostream>
#include <typeinfo>

#include <config/atframe_utils_build_feature.h>

#include "atframe/atapp.h"

#include "cli/shell_font.h"

namespace atapp {
LIBATAPP_MACRO_API module_impl::module_impl() : enabled_(true), actived_(false), owner_(nullptr) {}
LIBATAPP_MACRO_API module_impl::~module_impl() {
  if (nullptr != owner_) {
    on_unbind();
  }
}

LIBATAPP_MACRO_API void module_impl::on_bind() {}

LIBATAPP_MACRO_API void module_impl::on_unbind() {}

LIBATAPP_MACRO_API int module_impl::setup(app_conf &) { return 0; }

LIBATAPP_MACRO_API void module_impl::ready() {}

LIBATAPP_MACRO_API int module_impl::reload() { return 0; }

LIBATAPP_MACRO_API int module_impl::setup_log() { return 0; }

LIBATAPP_MACRO_API int module_impl::stop() { return 0; }

LIBATAPP_MACRO_API void module_impl::cleanup() {}

LIBATAPP_MACRO_API int module_impl::timeout() { return 0; }

LIBATAPP_MACRO_API int module_impl::tick() { return 0; }

LIBATAPP_MACRO_API const char *module_impl::name() const {
  if (auto_demangled_name_) {
    return auto_demangled_name_->get();
  }

#if defined(LIBATFRAME_UTILS_ENABLE_RTTI) && LIBATFRAME_UTILS_ENABLE_RTTI
  auto_demangled_name_.reset(new util::scoped_demangled_name(typeid(*this).name()));
  if (auto_demangled_name_) {
    return auto_demangled_name_->get();
  } else {
    return "atapp::module demangle symbol failed";
  }
#else
  return "atapp::module RTTI Unavailable";
#endif
}

LIBATAPP_MACRO_API uint64_t module_impl::get_app_id() const {
  if (nullptr == owner_) {
    return 0;
  }

  return owner_->get_id();
}

LIBATAPP_MACRO_API uint64_t module_impl::get_app_type_id() const {
  if (nullptr == owner_) {
    return 0;
  }

  return owner_->get_type_id();
}

LIBATAPP_MACRO_API bool module_impl::is_actived() const { return actived_; }

LIBATAPP_MACRO_API bool module_impl::is_enabled() const { return enabled_; }

LIBATAPP_MACRO_API bool module_impl::enable() {
  bool ret = enabled_;
  enabled_ = true;
  return ret;
}

LIBATAPP_MACRO_API bool module_impl::disable() {
  bool ret = enabled_;
  enabled_ = false;

  deactive();
  return ret;
}

LIBATAPP_MACRO_API bool module_impl::active() {
  bool ret = actived_;
  actived_ = true;
  return ret;
}

LIBATAPP_MACRO_API bool module_impl::deactive() {
  bool ret = actived_;
  actived_ = false;
  return ret;
}

LIBATAPP_MACRO_API app *module_impl::get_app() { return owner_; }

LIBATAPP_MACRO_API const app *module_impl::get_app() const { return owner_; }
}  // namespace atapp
