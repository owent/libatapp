// Copyright 2021 atframework
// Created by owent on 2016-05-18

#pragma once

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include <config/compiler_features.h>
#include <std/explicit_declare.h>

#include <common/demangle.h>

#include <memory>

#include "atframe/atapp_config.h"

namespace atapp {
class app;
struct app_conf;

class LIBATAPP_MACRO_API_SYMBOL_VISIBLE module_impl {
 protected:
  LIBATAPP_MACRO_API module_impl();
  LIBATAPP_MACRO_API virtual ~module_impl();

 private:
  UTIL_DESIGN_PATTERN_NOCOPYABLE(module_impl)
  UTIL_DESIGN_PATTERN_NOMOVABLE(module_impl)

 public:
  /**
   * @brief Call this callback when a module is added into atapp for the first time
   * @note Custom commands or options can be set up here
   */
  LIBATAPP_MACRO_API virtual void on_bind();

  /**
   * @brief Call this callback when a module is removed from atapp
   * @note If custom commands or options are set up on on_bind(), the related resource should be clear here
   */
  LIBATAPP_MACRO_API virtual void on_unbind();

  /**
   * @brief This callback is called after load configure and before initialization(inlcude log)
   * @note Changing the origin configure is allowed here(even app id and name)
   */
  LIBATAPP_MACRO_API virtual int setup(app_conf &conf);

  /**
   * @brief This callback is called to initialize a module
   * @note This function will be called before init when startup
   * @return error code or 0
   */
  virtual int init() = 0;

  /**
   * @brief This callback is called after all modules are initialized successfully and the atapp is ready to run
   * @note module can call get_app()->stop() to stop running.
   */
  LIBATAPP_MACRO_API virtual void ready();

  /**
   * @brief This callback is called after configure is reloaded
   * @note This function will be called before init when startup
   * @return error code or 0, return error code on startup will stop atapp to run
   */
  LIBATAPP_MACRO_API virtual int reload();

  /**
   * @brief where to setup custom log sink
   * @note This function will be called only once and before reload and init
   * @return error code or 0
   */
  LIBATAPP_MACRO_API virtual int setup_log();

  /**
   * @brief try to stop a module
   * @return if can't be stoped immadiately, return > 0, if there is a error, return < 0, otherwise 0
   * @note This callback may be called more than once, when the first return <= 0, this module will be disabled.
   */
  LIBATAPP_MACRO_API virtual int stop();

  /**
   * @brief cleanup a module
   * @note This callback only will be call once after all module stopped
   */
  LIBATAPP_MACRO_API virtual void cleanup();

  /**
   * @brief stop timeout callback
   * @note This callback be called if the module can not be stopped even in a long time.
   *       After this event, all module and atapp will be forced stopped.
   */
  LIBATAPP_MACRO_API virtual int timeout();

  /**
   * @brief get module name
   * @return module name
   */
  LIBATAPP_MACRO_API virtual const char *name() const;

  /**
   * @brief run tick handle and return active action number
   * @return active action number or error code
   */
  LIBATAPP_MACRO_API virtual int tick();

  /**
   * @brief get app id
   * @return app instance id
   */
  LIBATAPP_MACRO_API uint64_t get_app_id() const;

  /**
   * @brief get app type id
   * @return app instance id
   */
  LIBATAPP_MACRO_API uint64_t get_app_type_id() const;

  /**
   * @brief if this module is actived
   * @return true if module is actived
   */
  LIBATAPP_MACRO_API bool is_actived() const;

 protected:
  /**
   * @brief get owner atapp object
   * @return return owner atapp object, nullptr if not added
   */
  LIBATAPP_MACRO_API app *get_app();

  /**
   * @brief get owner atapp object
   * @return return owner atapp object, nullptr if not added
   */
  LIBATAPP_MACRO_API const app *get_app() const;

 protected:
  LIBATAPP_MACRO_API bool is_enabled() const;

  LIBATAPP_MACRO_API bool enable();

  LIBATAPP_MACRO_API bool disable();

  LIBATAPP_MACRO_API bool active();

  LIBATAPP_MACRO_API bool deactive();

 private:
  bool enabled_;
  bool actived_;
  app *owner_;

  mutable std::unique_ptr<util::scoped_demangled_name> auto_demangled_name_;

  friend class app;
};
}  // namespace atapp
