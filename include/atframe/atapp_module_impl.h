/**
 * atapp_module_impl.h
 *
 *  Created on: 2016年05月18日
 *      Author: owent
 */
#ifndef LIBATAPP_ATAPP_MODULE_IMPL_H
#define LIBATAPP_ATAPP_MODULE_IMPL_H

#pragma once

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include "config/compiler_features.h"
#include "std/explicit_declare.h"
#include "std/smart_ptr.h"

#include <common/demangle.h>

#include "atapp_config.h"

namespace atapp {
class app;

class module_impl {
 protected:
  LIBATAPP_MACRO_API module_impl();
  LIBATAPP_MACRO_API virtual ~module_impl();

 private:
  UTIL_DESIGN_PATTERN_NOCOPYABLE(module_impl)
  UTIL_DESIGN_PATTERN_NOMOVABLE(module_impl)

 public:
  virtual int init() = 0;
  LIBATAPP_MACRO_API virtual int reload();

  /**
   * @brief where to setup custom log sink
   * @note this will be called only once and before reload and init
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

 protected:
  /**
   * @brief get owner atapp object
   * @return return owner atapp object, NULL if not added
   */
  LIBATAPP_MACRO_API app *get_app();

  /**
   * @brief get owner atapp object
   * @return return owner atapp object, NULL if not added
   */
  LIBATAPP_MACRO_API const app *get_app() const;

 protected:
  UTIL_FORCEINLINE bool is_enabled() const { return enabled_; }

  LIBATAPP_MACRO_API bool enable();

  LIBATAPP_MACRO_API bool disable();

 private:
  bool enabled_;
  app *owner_;

  mutable std::unique_ptr<util::scoped_demangled_name> auto_demangled_name_;

  friend class app;
};
}  // namespace atapp

#endif
