/**
 * atapp_log_sink_maker.h
 *
 *  Created on: 2016年05月21日
 *      Author: owent
 */
#ifndef LIBATAPP_ATAPP_LOG_SINK_MAKER_H
#define LIBATAPP_ATAPP_LOG_SINK_MAKER_H

#pragma once

#include <string>

#include "log/log_wrapper.h"

#include "atapp_config.h"

namespace atapp {
    namespace protocol {
        class atapp_log;
        class atapp_log_category;
        class atapp_log_sink;
    }
}

namespace atapp {
    class log_sink_maker {
    public:
#if defined(UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES) && UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES
        using log_reg_t = std::function<util::log::log_wrapper::log_handler_t(util::log::log_wrapper &, int32_t, const ::atapp::protocol::atapp_log&, const ::atapp::protocol::atapp_log_category&, const ::atapp::protocol::atapp_log_sink&)>;
#else
        typedef std::function<util::log::log_wrapper::log_handler_t(util::log::log_wrapper &, int32_t, const ::atapp::protocol::atapp_log&, const ::atapp::protocol::atapp_log_category&, const ::atapp::protocol::atapp_log_sink&)> log_reg_t;
#endif

    private:
        log_sink_maker();
        ~log_sink_maker();

    public:
        static LIBATAPP_MACRO_API const std::string &get_file_sink_name();

        static LIBATAPP_MACRO_API log_reg_t get_file_sink_reg();

        static LIBATAPP_MACRO_API const std::string &get_stdout_sink_name();

        static LIBATAPP_MACRO_API log_reg_t get_stdout_sink_reg();

        static LIBATAPP_MACRO_API const std::string &get_stderr_sink_name();

        static LIBATAPP_MACRO_API log_reg_t get_stderr_sink_reg();
    };
}

#endif
