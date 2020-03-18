#include <iostream>

#include "cli/shell_font.h"
#include "log/log_sink_file_backend.h"

#include "atframe/atapp_log_sink_maker.h"


namespace atapp {
    namespace detail {
        static util::log::log_wrapper::log_handler_t _log_sink_file(const std::string & /*sink_name*/, util::log::log_wrapper & /*logger*/, uint32_t /*index*/,
                                                                    util::config::ini_value &ini_cfg) {
            std::string file_pattern = ini_cfg["file"].as_cpp_string();
            if (file_pattern.empty()) {
                file_pattern = "server.%N.log";
            }
            size_t max_file_size = 0; // 64KB
            uint32_t rotate_size = 0; // 0-9

            util::log::log_sink_file_backend file_sink;
            if (ini_cfg.get_children().end() != ini_cfg.get_children().find("rotate")) {
                util::config::ini_value &rotate_conf = ini_cfg["rotate"];
                max_file_size = rotate_conf["size"].as<size_t>();
                rotate_size = rotate_conf["number"].as_uint32();
            }

            if (0 == max_file_size) {
                max_file_size = 65536; // 64KB
            }

            if (0 == rotate_size) {
                rotate_size = 10; // 0-9
            }

            file_sink.set_file_pattern(file_pattern);
            file_sink.set_max_file_size(max_file_size);
            file_sink.set_rotate_size(rotate_size);

            if (ini_cfg.get_children().end() != ini_cfg.get_children().find("auto_flush")) {
                uint32_t auto_flush = ini_cfg["auto_flush"].as_uint32();
                file_sink.set_auto_flush(auto_flush);
            }

            if (ini_cfg.get_children().end() != ini_cfg.get_children().find("flush_interval")) {
                util::config::duration_value flush_interval = ini_cfg["flush_interval"].as_duration();
                file_sink.set_flush_interval(flush_interval.sec);
            }

            if (ini_cfg.get_children().end() != ini_cfg.get_children().find("writing_alias")) {
                file_sink.set_writing_alias_pattern(ini_cfg["writing_alias"].as_cpp_string());
            }

            return file_sink;
        }

        static void _log_sink_stdout_handle(const util::log::log_wrapper::caller_info_t &, const char *content, size_t content_size) {
            std::cout.write(content, content_size);
            std::cout << std::endl;
        }

        static util::log::log_wrapper::log_handler_t _log_sink_stdout(const std::string & /*sink_name*/, util::log::log_wrapper & /*logger*/,
                                                                      uint32_t /*index*/, util::config::ini_value & /*ini_cfg*/) {
            return _log_sink_stdout_handle;
        }

        static void _log_sink_stderr_handle(const util::log::log_wrapper::caller_info_t & /*caller*/, const char *content, size_t /*content_size*/) {
            util::cli::shell_stream ss(std::cerr);
            ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << content << std::endl;
        }

        static util::log::log_wrapper::log_handler_t _log_sink_stderr(const std::string & /*sink_name*/, util::log::log_wrapper & /*logger*/,
                                                                      uint32_t /*index*/, util::config::ini_value & /*ini_cfg*/) {
            return _log_sink_stderr_handle;
        }
    } // namespace detail

    log_sink_maker::log_sink_maker() {}

    log_sink_maker::~log_sink_maker() {}


    LIBATAPP_MACRO_API const std::string &log_sink_maker::get_file_sink_name() {
        static std::string ret = "file";
        return ret;
    }

    LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_file_sink_reg() { return detail::_log_sink_file; }

    LIBATAPP_MACRO_API const std::string &log_sink_maker::get_stdout_sink_name() {
        static std::string ret = "stdout";
        return ret;
    }

    LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_stdout_sink_reg() { return detail::_log_sink_stdout; }

    LIBATAPP_MACRO_API const std::string &log_sink_maker::get_stderr_sink_name() {
        static std::string ret = "stderr";
        return ret;
    }

    LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_stderr_sink_reg() { return detail::_log_sink_stderr; }
} // namespace atapp
