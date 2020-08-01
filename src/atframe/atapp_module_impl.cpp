#include <iostream>
#include <signal.h>
#include <typeinfo>

#include "atframe/atapp.h"

#include "cli/shell_font.h"


namespace atapp {
    LIBATAPP_MACRO_API module_impl::module_impl() : enabled_(true), owner_(NULL) {}
    LIBATAPP_MACRO_API module_impl::~module_impl() {}

    LIBATAPP_MACRO_API int module_impl::reload() { return 0; }

    LIBATAPP_MACRO_API int module_impl::setup_log() { return 0; }

    LIBATAPP_MACRO_API int module_impl::stop() { return 0; }

    LIBATAPP_MACRO_API void module_impl::cleanup() {}

    LIBATAPP_MACRO_API int module_impl::timeout() { return 0; }

    LIBATAPP_MACRO_API int module_impl::tick() { return 0; }

    LIBATAPP_MACRO_API uint64_t module_impl::get_app_id() const {
        if (NULL == owner_) {
            return 0;
        }

        return owner_->get_id();
    }

    LIBATAPP_MACRO_API uint64_t module_impl::get_app_type_id() const {
        if (NULL == owner_) {
            return 0;
        }

        return owner_->get_type_id();
    }

    LIBATAPP_MACRO_API const char *module_impl::name() const {
        const char *ret = typeid(*this).name();
        if (NULL == ret) {
            return "RTTI Unavailable";
        }

        // some compiler will generate number to mark the type
        while (ret && *ret >= '0' && *ret <= '9') {
            ++ret;
        }
        return ret;
    }

    LIBATAPP_MACRO_API bool module_impl::enable() {
        bool ret = enabled_;
        enabled_ = true;
        return ret;
    }

    LIBATAPP_MACRO_API bool module_impl::disable() {
        bool ret = enabled_;
        enabled_ = false;
        return ret;
    }

    LIBATAPP_MACRO_API app *module_impl::get_app() { return owner_; }

    LIBATAPP_MACRO_API const app *module_impl::get_app() const { return owner_; }
} // namespace atapp
