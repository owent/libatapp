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

    class LIBATAPP_MACRO_API module_impl {
    protected:
        module_impl();
        virtual ~module_impl();

    private:
        UTIL_DESIGN_PATTERN_NOCOPYABLE(module_impl)
        UTIL_DESIGN_PATTERN_NOMOVABLE(module_impl)

    public:
        /**
         * @brief Call this callback when a module is added into atapp for the first time
         * @note Custom commands or options can be set up here
         */
        virtual void on_bind();

        /**
         * @brief Call this callback when a module is removed from atapp
         * @note If custom commands or options are set up on on_bind(), the related resource should be clear here
         */
        virtual void on_unbind();

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
        virtual void ready();

        /**
         * @brief This callback is called after configure is reloaded
         * @note This function will be called before init when startup
         * @return error code or 0, return error code on startup will stop atapp to run
         */
        virtual int reload();

        /**
         * @brief where to setup custom log sink
         * @note This function will be called only once and before reload and init
         * @return error code or 0
         */
        virtual int setup_log();

        /**
         * @brief try to stop a module
         * @return if can't be stoped immadiately, return > 0, if there is a error, return < 0, otherwise 0
         * @note This callback may be called more than once, when the first return <= 0, this module will be disabled.
         */
        virtual int stop();

        /**
         * @brief cleanup a module
         * @note This callback only will be call once after all module stopped
         */
        virtual void cleanup();

        /**
         * @brief stop timeout callback
         * @note This callback be called if the module can not be stopped even in a long time.
         *       After this event, all module and atapp will be forced stopped.
         */
        virtual int timeout();

        /**
         * @brief get module name
         * @return module name
         */
        virtual const char *name() const;

        /**
         * @brief run tick handle and return active action number
         * @return active action number or error code
         */
        virtual int tick();

        /**
         * @brief get app id
         * @return app instance id
         */
        uint64_t get_app_id() const;

        /**
         * @brief get app type id
         * @return app instance id
         */
        uint64_t get_app_type_id() const;

    protected:
        /**
         * @brief get owner atapp object
         * @return return owner atapp object, NULL if not added
         */
        app *get_app();

        /**
         * @brief get owner atapp object
         * @return return owner atapp object, NULL if not added
         */
        const app *get_app() const;

    protected:
        UTIL_FORCEINLINE bool is_enabled() const { return enabled_; }

        bool enable();

        bool disable();

    private:
        bool enabled_;
        app *owner_;

        mutable std::unique_ptr<util::scoped_demangled_name> auto_demangled_name_;

        friend class app;
    };
} // namespace atapp

#endif
