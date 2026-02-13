// Copyright 2026 atframework
//
// Created by owent on 2016-05-21

#pragma once

#include <string>

#include <gsl/select-gsl.h>
#include <log/log_wrapper.h>

#include "atframe/atapp_config.h"

namespace atframework {
namespace atapp {
namespace protocol {
class atapp_log;
class atapp_log_category;
class atapp_log_sink;
}  // namespace protocol
}  // namespace atapp
}  // namespace atframework

LIBATAPP_MACRO_NAMESPACE_BEGIN

class log_sink_maker {
 public:
  using log_reg_t = std::function<atfw::util::log::log_wrapper::log_handler_t(
      atfw::util::log::log_wrapper &, int32_t, const ::atframework::atapp::protocol::atapp_log &,
      const ::atframework::atapp::protocol::atapp_log_category &,
      const ::atframework::atapp::protocol::atapp_log_sink &)>;

 private:
  log_sink_maker();
  ~log_sink_maker();

 public:
  static LIBATAPP_MACRO_API gsl::string_view get_file_sink_name();

  static LIBATAPP_MACRO_API log_reg_t get_file_sink_reg();

  static LIBATAPP_MACRO_API gsl::string_view get_stdout_sink_name();

  static LIBATAPP_MACRO_API log_reg_t get_stdout_sink_reg();

  static LIBATAPP_MACRO_API gsl::string_view get_stderr_sink_name();

  static LIBATAPP_MACRO_API log_reg_t get_stderr_sink_reg();

  static LIBATAPP_MACRO_API gsl::string_view get_syslog_sink_name();

  static LIBATAPP_MACRO_API log_reg_t get_syslog_sink_reg();
};
LIBATAPP_MACRO_NAMESPACE_END

