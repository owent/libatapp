// Copyright 2026 atframework
// Windows Minidump integration for atapp

#pragma once

#include "atframe/atapp_config.h"

#include <string>

namespace atframework {
namespace atapp {
namespace protocol {
class atapp_debug;
}
}  // namespace atapp
}  // namespace atframework

LIBATAPP_MACRO_NAMESPACE_BEGIN

LIBATAPP_MACRO_API void setup_windows_minidump(const ::atframework::atapp::protocol::atapp_debug &debug_config,
                                               const std::string &app_name);
LIBATAPP_MACRO_API void cleanup_windows_minidump();

LIBATAPP_MACRO_NAMESPACE_END
