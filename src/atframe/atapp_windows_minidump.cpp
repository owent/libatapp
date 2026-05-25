// Copyright 2026 atframework
// Windows Minidump integration for atapp

#include "atframe/atapp_windows_minidump.h"

// clang-format off
#include <config/compiler/protobuf_prefix.h>
// clang-format on

#include <atframe/atapp_conf.pb.h>

// clang-format off
#include <config/compiler/protobuf_suffix.h>
// clang-format on

#include <log/log_wrapper.h>

#ifdef _WIN32
#  include <common/file_system.h>
#  include <common/string_oprs.h>

// clang-format off
#  include <windows.h>
#  include <dbghelp.h>
// clang-format on
#  pragma comment(lib, "dbghelp.lib")

#  include <cstdio>
#  include <cstring>
#endif

LIBATAPP_MACRO_NAMESPACE_BEGIN

#ifdef _WIN32

namespace {
struct minidump_config_snapshot {
  const char *path;
  size_t path_len;
  const char *app_name;
  size_t app_name_len;
  DWORD flags;
};

struct minidump_state {
  SRWLOCK lock;
  std::string path;
  std::string app_name;
  DWORD flags;
  LPTOP_LEVEL_EXCEPTION_FILTER previous_filter;
  bool filter_registered;

  std::atomic<const minidump_config_snapshot *> active_snapshot;

  minidump_state()
      : lock(SRWLOCK_INIT),
        flags(MiniDumpNormal),
        previous_filter(nullptr),
        filter_registered(false),
        active_snapshot(nullptr) {}
};

static minidump_state &get_minidump_state() {
  static minidump_state inst;
  return inst;
}

static void update_snapshot(minidump_state &state) {
  minidump_config_snapshot *new_snap = nullptr;
  if (!state.path.empty()) {
    new_snap = new minidump_config_snapshot();
    new_snap->path = state.path.c_str();
    new_snap->path_len = state.path.size();
    new_snap->app_name = state.app_name.c_str();
    new_snap->app_name_len = state.app_name.size();
    new_snap->flags = state.flags;
  }

  const minidump_config_snapshot *old_snap = state.active_snapshot.exchange(new_snap, std::memory_order_acq_rel);
  delete old_snap;
}

static DWORD get_minidump_type_flags(atapp::protocol::atapp_debug_windows_minidump_mode mode) {
  switch (mode) {
    case atapp::protocol::ATAPP_DEBUG_WINDOWS_MINIDUMP_MODE_WITH_DATA_SEGS:
      return MiniDumpWithDataSegs | MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo |
             MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory;
    case atapp::protocol::ATAPP_DEBUG_WINDOWS_MINIDUMP_MODE_WITH_FULL_MEMORY:
      return MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
             MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData |
             MiniDumpWithFullAuxiliaryState;
    case atapp::protocol::ATAPP_DEBUG_WINDOWS_MINIDUMP_MODE_MINI:
    default:
      return MiniDumpWithDataSegs | MiniDumpWithThreadInfo;
  }
}

static bool ensure_directory_exists(const std::string &dir_path) {
  if (dir_path.empty()) {
    return false;
  }

  DWORD attrs = GetFileAttributesA(dir_path.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    return true;
  }

  std::string path_copy = dir_path;
  for (size_t i = 0; i < path_copy.size(); ++i) {
    if (path_copy[i] == '/' || path_copy[i] == '\\') {
      if (i == 0) {
        continue;
      }
      if (i >= 2 && path_copy[i - 1] == ':' && (path_copy[i - 2] >= 'A' && path_copy[i - 2] <= 'z')) {
        continue;
      }
      char saved = path_copy[i];
      path_copy[i] = '\0';
      DWORD a = GetFileAttributesA(path_copy.c_str());
      if (a == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryA(path_copy.c_str(), nullptr)) {
          DWORD err = GetLastError();
          if (err != ERROR_ALREADY_EXISTS) {
            FWLOGERROR("Windows minidump: Failed to create directory '{}', error={}", path_copy.c_str(), err);
          }
        }
      }
      path_copy[i] = saved;
    }
  }
  if (!CreateDirectoryA(path_copy.c_str(), nullptr)) {
    DWORD err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS) {
      FWLOGERROR("Windows minidump: Failed to create directory '{}', error={}", path_copy.c_str(), err);
      return false;
    }
  }
  return true;
}

static LONG WINAPI minidump_exception_filter(EXCEPTION_POINTERS *exc_ptr) {
  minidump_state &state = get_minidump_state();
  const minidump_config_snapshot *snap = state.active_snapshot.load(std::memory_order_acquire);
  if (snap == nullptr || snap->path == nullptr || snap->path_len == 0) {
    if (state.previous_filter != nullptr) {
      return state.previous_filter(exc_ptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  DWORD exception_code = 0;
  if (exc_ptr != nullptr && exc_ptr->ExceptionRecord != nullptr) {
    exception_code = exc_ptr->ExceptionRecord->ExceptionCode;
  }

  SYSTEMTIME st;
  GetLocalTime(&st);

  char dump_filename[MAX_PATH];
  int written = snprintf(dump_filename, sizeof(dump_filename), "%s\\%s_%04d%02d%02d_%02d%02d%02d_%lu_%08lX.dmp",
                         snap->path, snap->app_name != nullptr ? snap->app_name : "unknown", st.wYear, st.wMonth,
                         st.wDay, st.wHour, st.wMinute, st.wSecond, static_cast<unsigned long>(GetCurrentProcessId()),
                         static_cast<unsigned long>(exception_code));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(dump_filename)) {
    OutputDebugStringA("Windows minidump: Failed to generate dump filename (path too long or buffer overflow)\r\n");
    if (state.previous_filter != nullptr) {
      return state.previous_filter(exc_ptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HANDLE h_file = CreateFileA(dump_filename, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h_file == INVALID_HANDLE_VALUE) {
    OutputDebugStringA("Windows minidump: Failed to create dump file\r\n");
    if (state.previous_filter != nullptr) {
      return state.previous_filter(exc_ptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = exc_ptr;
  mei.ClientPointers = FALSE;

  BOOL dump_result =
      MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h_file, static_cast<MINIDUMP_TYPE>(snap->flags),
                        exc_ptr != nullptr ? &mei : nullptr, nullptr, nullptr);

  CloseHandle(h_file);

  if (!dump_result) {
    DeleteFileA(dump_filename);
  }

  if (state.previous_filter != nullptr) {
    return state.previous_filter(exc_ptr);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

LIBATAPP_MACRO_API void setup_windows_minidump(const protocol::atapp_debug &debug_config, const std::string &app_name) {
  minidump_state &state = get_minidump_state();

  {
    std::string new_path = debug_config.windows_minidump_path();
    if (new_path.empty()) {
      AcquireSRWLockExclusive(&state.lock);
      state.path.clear();
      state.app_name.clear();
      update_snapshot(state);
      ReleaseSRWLockExclusive(&state.lock);
      FWLOGINFO("Windows minidump is disabled (no path configured)");
      return;
    }

    new_path = atfw::util::file_system::get_abs_path(new_path.c_str());
    if (!new_path.empty() && new_path.back() != '\\' && new_path.back() != '/') {
      new_path += '\\';
    }

    if (!ensure_directory_exists(new_path)) {
      FWLOGERROR("Windows minidump: Failed to ensure directory exists for '{}'", new_path);
    }

    DWORD new_flags = get_minidump_type_flags(debug_config.windows_minidump_mode());

    AcquireSRWLockExclusive(&state.lock);
    state.path = std::move(new_path);
    state.app_name = app_name.empty() ? "unknown" : app_name;
    state.flags = new_flags;

    if (!state.filter_registered) {
      state.previous_filter = SetUnhandledExceptionFilter(minidump_exception_filter);
      state.filter_registered = true;
      ReleaseSRWLockExclusive(&state.lock);
      FWLOGINFO("Windows minidump exception filter registered");
    } else {
      ReleaseSRWLockExclusive(&state.lock);
    }

    update_snapshot(state);

    FWLOGINFO("Windows minidump configured: path={}, mode={}", state.path,
              static_cast<int>(debug_config.windows_minidump_mode()));
  }
}

LIBATAPP_MACRO_API void cleanup_windows_minidump() {
  minidump_state &state = get_minidump_state();
  AcquireSRWLockExclusive(&state.lock);
  if (state.filter_registered) {
    if (state.previous_filter != nullptr) {
      SetUnhandledExceptionFilter(state.previous_filter);
      state.previous_filter = nullptr;
    }
    state.filter_registered = false;
  }

  state.path.clear();
  state.app_name.clear();
  state.flags = MiniDumpWithDataSegs | MiniDumpWithThreadInfo;
  update_snapshot(state);
  ReleaseSRWLockExclusive(&state.lock);
}

#else

LIBATAPP_MACRO_API void setup_windows_minidump(const protocol::atapp_debug &, const std::string &) {}

LIBATAPP_MACRO_API void cleanup_windows_minidump() {}

#endif  // _WIN32

LIBATAPP_MACRO_NAMESPACE_END
