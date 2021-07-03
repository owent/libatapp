// Copyright 2021 atframework
// Created by owent

#include "atframe/atapp_log_sink_maker.h"

#include <atframe/atapp_conf.h>

#include <iostream>

#include "cli/shell_font.h"
#include "log/log_sink_file_backend.h"

namespace atapp {
namespace detail {
static util::log::log_wrapper::log_handler_t _log_sink_file(util::log::log_wrapper& /*logger*/, int32_t /*index*/,
                                                            const ::atapp::protocol::atapp_log& /*log_cfg*/,
                                                            const ::atapp::protocol::atapp_log_category& /*cat_cfg*/,
                                                            const ::atapp::protocol::atapp_log_sink& sink_cfg) {
  std::string file_pattern = sink_cfg.log_backend_file().file();
  if (file_pattern.empty()) {
    file_pattern = "server.%N.log";
  }
  size_t max_file_size = static_cast<size_t>(sink_cfg.log_backend_file().rotate().size());
  uint32_t rotate_size = static_cast<uint32_t>(sink_cfg.log_backend_file().rotate().number());

  util::log::log_sink_file_backend file_sink;
  if (0 == max_file_size) {
    max_file_size = 262144;  // 256KB
  }

  if (0 == rotate_size) {
    rotate_size = 10;  // 0-9
  }

  file_sink.set_file_pattern(file_pattern);
  file_sink.set_max_file_size(max_file_size);
  file_sink.set_rotate_size(rotate_size);

  file_sink.set_auto_flush(
      util::log::log_formatter::get_level_by_name(sink_cfg.log_backend_file().auto_flush().c_str()));
  file_sink.set_flush_interval(static_cast<time_t>(sink_cfg.log_backend_file().flush_interval().seconds()));
  file_sink.set_writing_alias_pattern(sink_cfg.log_backend_file().writing_alias());

  return file_sink;
}

static void _log_sink_stdout_handle(const util::log::log_wrapper::caller_info_t& caller, const char* content,
                                    size_t content_size) {
  if (caller.level_id <= util::log::log_formatter::level_t::LOG_LW_ERROR) {
    util::cli::shell_stream ss(std::cout);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << content << std::endl;
  } else if (caller.level_id == util::log::log_formatter::level_t::LOG_LW_WARNING) {
    util::cli::shell_stream ss(std::cout);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << content << std::endl;
  } else if (caller.level_id == util::log::log_formatter::level_t::LOG_LW_INFO) {
    util::cli::shell_stream ss(std::cout);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_GREEN << content << std::endl;
  } else {
    std::cout.write(content, content_size);
    std::cout << std::endl;
  }
}

static util::log::log_wrapper::log_handler_t _log_sink_stdout(util::log::log_wrapper& /*logger*/, int32_t /*index*/,
                                                              const ::atapp::protocol::atapp_log& /*log_cfg*/,
                                                              const ::atapp::protocol::atapp_log_category& /*cat_cfg*/,
                                                              const ::atapp::protocol::atapp_log_sink& /*sink_cfg*/) {
  return _log_sink_stdout_handle;
}

static void _log_sink_stderr_handle(const util::log::log_wrapper::caller_info_t& caller, const char* content,
                                    size_t content_size) {
  if (caller.level_id <= util::log::log_formatter::level_t::LOG_LW_ERROR) {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_RED << content << std::endl;
  } else if (caller.level_id == util::log::log_formatter::level_t::LOG_LW_WARNING) {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << content << std::endl;
  } else if (caller.level_id == util::log::log_formatter::level_t::LOG_LW_INFO) {
    util::cli::shell_stream ss(std::cerr);
    ss() << util::cli::shell_font_style::SHELL_FONT_COLOR_GREEN << content << std::endl;
  } else {
    std::cerr.write(content, content_size);
    std::cerr << std::endl;
  }
}

static util::log::log_wrapper::log_handler_t _log_sink_stderr(util::log::log_wrapper& /*logger*/, int32_t /*index*/,
                                                              const ::atapp::protocol::atapp_log& /*log_cfg*/,
                                                              const ::atapp::protocol::atapp_log_category& /*cat_cfg*/,
                                                              const ::atapp::protocol::atapp_log_sink& /*sink_cfg*/) {
  return _log_sink_stderr_handle;
}
}  // namespace detail

log_sink_maker::log_sink_maker() {}

log_sink_maker::~log_sink_maker() {}

LIBATAPP_MACRO_API const std::string& log_sink_maker::get_file_sink_name() {
  static std::string ret = "file";
  return ret;
}

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_file_sink_reg() { return detail::_log_sink_file; }

LIBATAPP_MACRO_API const std::string& log_sink_maker::get_stdout_sink_name() {
  static std::string ret = "stdout";
  return ret;
}

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_stdout_sink_reg() { return detail::_log_sink_stdout; }

LIBATAPP_MACRO_API const std::string& log_sink_maker::get_stderr_sink_name() {
  static std::string ret = "stderr";
  return ret;
}

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_stderr_sink_reg() { return detail::_log_sink_stderr; }
}  // namespace atapp
