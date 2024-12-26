// Copyright 2021 atframework
// Created by owent

#include "atframe/atapp_log_sink_maker.h"

#include <atframe/atapp_conf.h>

#include <iostream>

#include "cli/shell_font.h"
#include "common/string_oprs.h"
#include "log/log_sink_file_backend.h"
#include "log/log_sink_syslog_backend.h"

#if defined(LOG_SINK_ENABLE_SYSLOG_SUPPORT) && LOG_SINK_ENABLE_SYSLOG_SUPPORT
#  include <syslog.h>
#endif

namespace atapp {
namespace detail {
static atfw::util::log::log_wrapper::log_handler_t _log_sink_file(
    atfw::util::log::log_wrapper& /*logger*/, int32_t /*index*/, const ::atapp::protocol::atapp_log& /*log_cfg*/,
    const ::atapp::protocol::atapp_log_category& /*cat_cfg*/, const ::atapp::protocol::atapp_log_sink& sink_cfg) {
  std::string file_pattern = sink_cfg.log_backend_file().file();
  if (file_pattern.empty()) {
    file_pattern = "server.%N.log";
  }
  size_t max_file_size = static_cast<size_t>(sink_cfg.log_backend_file().rotate().size());
  uint32_t rotate_size = static_cast<uint32_t>(sink_cfg.log_backend_file().rotate().number());

  atfw::util::log::log_sink_file_backend file_sink;
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
      atfw::util::log::log_formatter::get_level_by_name(sink_cfg.log_backend_file().auto_flush().c_str()));
  file_sink.set_flush_interval(static_cast<time_t>(sink_cfg.log_backend_file().flush_interval().seconds()));
  file_sink.set_writing_alias_pattern(sink_cfg.log_backend_file().writing_alias());

  return file_sink;
}

static void _log_sink_stdout_handle(const atfw::util::log::log_wrapper::caller_info_t& caller, const char* content,
                                    size_t content_size) {
  if (caller.level_id <= atfw::util::log::log_formatter::level_t::LOG_LW_ERROR) {
    atfw::util::cli::shell_stream ss(std::cout);
    ss() << atfw::util::cli::shell_font_style::SHELL_FONT_COLOR_RED << content << std::endl;
  } else if (caller.level_id == atfw::util::log::log_formatter::level_t::LOG_LW_WARNING) {
    atfw::util::cli::shell_stream ss(std::cout);
    ss() << atfw::util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << content << std::endl;
  } else if (caller.level_id == atfw::util::log::log_formatter::level_t::LOG_LW_INFO) {
    atfw::util::cli::shell_stream ss(std::cout);
    ss() << atfw::util::cli::shell_font_style::SHELL_FONT_COLOR_GREEN << content << std::endl;
  } else {
    std::cout.write(content, content_size);
    std::cout << std::endl;
  }
}

static atfw::util::log::log_wrapper::log_handler_t _log_sink_stdout(
    atfw::util::log::log_wrapper& /*logger*/, int32_t /*index*/, const ::atapp::protocol::atapp_log& /*log_cfg*/,
    const ::atapp::protocol::atapp_log_category& /*cat_cfg*/, const ::atapp::protocol::atapp_log_sink& /*sink_cfg*/) {
  return _log_sink_stdout_handle;
}

static void _log_sink_stderr_handle(const atfw::util::log::log_wrapper::caller_info_t& caller, const char* content,
                                    size_t content_size) {
  if (caller.level_id <= atfw::util::log::log_formatter::level_t::LOG_LW_ERROR) {
    atfw::util::cli::shell_stream ss(std::cerr);
    ss() << atfw::util::cli::shell_font_style::SHELL_FONT_COLOR_RED << content << std::endl;
  } else if (caller.level_id == atfw::util::log::log_formatter::level_t::LOG_LW_WARNING) {
    atfw::util::cli::shell_stream ss(std::cerr);
    ss() << atfw::util::cli::shell_font_style::SHELL_FONT_COLOR_YELLOW << content << std::endl;
  } else if (caller.level_id == atfw::util::log::log_formatter::level_t::LOG_LW_INFO) {
    atfw::util::cli::shell_stream ss(std::cerr);
    ss() << atfw::util::cli::shell_font_style::SHELL_FONT_COLOR_GREEN << content << std::endl;
  } else {
    std::cerr.write(content, content_size);
    std::cerr << std::endl;
  }
}

static atfw::util::log::log_wrapper::log_handler_t _log_sink_stderr(
    atfw::util::log::log_wrapper& /*logger*/, int32_t /*index*/, const ::atapp::protocol::atapp_log& /*log_cfg*/,
    const ::atapp::protocol::atapp_log_category& /*cat_cfg*/, const ::atapp::protocol::atapp_log_sink& /*sink_cfg*/) {
  return _log_sink_stderr_handle;
}

static atfw::util::log::log_wrapper::log_handler_t _log_sink_syslog(
    atfw::util::log::log_wrapper& /*logger*/, int32_t /*index*/, const ::atapp::protocol::atapp_log& /*log_cfg*/,
    const ::atapp::protocol::atapp_log_category& /*cat_cfg*/, const ::atapp::protocol::atapp_log_sink& sink_cfg) {
  const ::atapp::protocol::atapp_log_sink_syslog& syslog_conf = sink_cfg.log_backend_syslog();
#if defined(LOG_SINK_ENABLE_SYSLOG_SUPPORT) && LOG_SINK_ENABLE_SYSLOG_SUPPORT
  int options = 0;
  for (const std::string& option : syslog_conf.options()) {
    if (0 == UTIL_STRFUNC_STRCASE_CMP("cons", option.c_str())) {
      options |= LOG_CONS;
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP("ndelay", option.c_str())) {
      options |= LOG_NDELAY;
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP("nowait", option.c_str())) {
      options |= LOG_NOWAIT;
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP("odely", option.c_str())) {
      options |= LOG_ODELAY;
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP("perror", option.c_str())) {
      options |= LOG_PERROR;
    } else if (0 == UTIL_STRFUNC_STRCASE_CMP("pid", option.c_str())) {
      options |= LOG_PID;
    }
  }
  if (0 == options) {
    options = LOG_PID;
  }
  int facility;
  if (0 == UTIL_STRFUNC_STRCASE_CMP("cons", syslog_conf.facility().c_str())) {
    facility = LOG_AUTH;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("ndelay", syslog_conf.facility().c_str())) {
    facility = LOG_AUTHPRIV;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("nowait", syslog_conf.facility().c_str())) {
    facility = LOG_CRON;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("odely", syslog_conf.facility().c_str())) {
    facility = LOG_DAEMON;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("perror", syslog_conf.facility().c_str())) {
    facility = LOG_FTP;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("pid", syslog_conf.facility().c_str())) {
    facility = LOG_KERN;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("lpr", syslog_conf.facility().c_str())) {
    facility = LOG_LPR;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("mail", syslog_conf.facility().c_str())) {
    facility = LOG_MAIL;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("syslog", syslog_conf.facility().c_str())) {
    facility = LOG_SYSLOG;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("user", syslog_conf.facility().c_str())) {
    facility = LOG_USER;
  } else if (0 == UTIL_STRFUNC_STRCASE_CMP("uucp", syslog_conf.facility().c_str())) {
    facility = LOG_UUCP;
  } else {
    facility = atfw::util::string::to_int<int>(syslog_conf.facility().c_str());
    if (0 == facility) {
      facility = LOG_USER;
    }
  }

  return atfw::util::log::log_sink_syslog_backend(syslog_conf.ident().empty() ? nullptr : syslog_conf.ident().c_str(),
                                                  options, facility);

#else
  for (const std::string& option : syslog_conf.options()) {
    if (0 == UTIL_STRFUNC_STRCASE_CMP("perror", option.c_str())) {
      return _log_sink_stderr_handle;
    }
  }

  return _log_sink_stdout_handle;
#endif
}

}  // namespace detail

log_sink_maker::log_sink_maker() {}

log_sink_maker::~log_sink_maker() {}

LIBATAPP_MACRO_API gsl::string_view log_sink_maker::get_file_sink_name() { return "file"; }

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_file_sink_reg() { return detail::_log_sink_file; }

LIBATAPP_MACRO_API gsl::string_view log_sink_maker::get_stdout_sink_name() { return "stdout"; }

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_stdout_sink_reg() { return detail::_log_sink_stdout; }

LIBATAPP_MACRO_API gsl::string_view log_sink_maker::get_stderr_sink_name() { return "stderr"; }

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_stderr_sink_reg() { return detail::_log_sink_stderr; }

LIBATAPP_MACRO_API gsl::string_view log_sink_maker::get_syslog_sink_name() { return "syslog"; }

LIBATAPP_MACRO_API log_sink_maker::log_reg_t log_sink_maker::get_syslog_sink_reg() { return detail::_log_sink_syslog; }

}  // namespace atapp
