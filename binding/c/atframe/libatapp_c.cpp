
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/compiler_message.h"
#include "common/string_oprs.h"
#include "libatapp_c.h"

#include <atframe/atapp.h>

#include <libatbus.h>
#include <libatbus_protocol.h>

#define ATAPP_CONTEXT(x) ((::atapp::app *)(x))
#define ATAPP_CONTEXT_IS_NULL(x) (nullptr == (x))

#define ATAPP_MESSAGE(x) ((const ::atapp::app::message_t *)(x[1]))
#define ATAPP_MESSAGE_IS_NULL(x) (nullptr == (x))

#define ATAPP_SENDER(x) ((const ::atapp::app::message_sender_t *)(x[0]))

namespace detail {
struct libatapp_c_on_msg_functor {
  libatapp_c_on_msg_functor(libatapp_c_on_msg_fn_t fn, void *priv_data) : callee_(fn), private_data_(priv_data) {}

  int operator()(::atapp::app &self, const ::atapp::app::message_sender_t &source, const ::atapp::app::message_t &msg) {
    if (nullptr == callee_) {
      return 0;
    }

    libatapp_c_context ctx;
    libatapp_c_message m;

    ctx = &self;
    m[0] = &source;
    m[1] = &msg;
    return (*callee_)(ctx, m, msg.data, static_cast<uint64_t>(msg.data_size), private_data_);
  }

  libatapp_c_on_msg_fn_t callee_;
  void *private_data_;
};

struct libatapp_c_on_send_fail_functor {
  libatapp_c_on_send_fail_functor(libatapp_c_on_send_fail_fn_t fn, void *priv_data)
      : callee_(fn), private_data_(priv_data) {}

  int operator()(::atapp::app &self, const ::atapp::app::message_sender_t &source, const ::atapp::app::message_t &msg,
                 int32_t) {
    if (nullptr == callee_) {
      return 0;
    }

    libatapp_c_context ctx;
    libatapp_c_message m;

    ctx = &self;
    m[0] = &source;
    m[1] = &msg;
    return (*callee_)(ctx, static_cast<uint64_t>(self.get_id()), static_cast<uint64_t>(source.id), m, private_data_);
  }

  libatapp_c_on_send_fail_fn_t callee_;
  void *private_data_;
};

struct libatapp_c_on_connected_functor {
  libatapp_c_on_connected_functor(libatapp_c_on_connected_fn_t fn, void *priv_data)
      : callee_(fn), private_data_(priv_data) {}

  int operator()(::atapp::app &self, ::atbus::endpoint &ep, int status) {
    if (nullptr == callee_) {
      return 0;
    }

    libatapp_c_context ctx;
    ctx = &self;
    return (*callee_)(ctx, static_cast<uint64_t>(ep.get_id()), static_cast<int32_t>(status), private_data_);
  }

  libatapp_c_on_connected_fn_t callee_;
  void *private_data_;
};

struct libatapp_c_on_disconnected_functor {
  libatapp_c_on_disconnected_functor(libatapp_c_on_disconnected_fn_t fn, void *priv_data)
      : callee_(fn), private_data_(priv_data) {}

  int operator()(::atapp::app &self, ::atbus::endpoint &ep, int status) {
    if (nullptr == callee_) {
      return 0;
    }

    libatapp_c_context ctx;
    ctx = &self;
    return (*callee_)(ctx, static_cast<uint64_t>(ep.get_id()), static_cast<int32_t>(status), private_data_);
  }

  libatapp_c_on_disconnected_fn_t callee_;
  void *private_data_;
};

struct libatapp_c_on_all_module_inited_functor {
  libatapp_c_on_all_module_inited_functor(libatapp_c_on_all_module_inited_fn_t fn, void *priv_data)
      : callee_(fn), private_data_(priv_data) {}

  int operator()(::atapp::app &self) {
    if (nullptr == callee_) {
      return 0;
    }

    libatapp_c_context ctx;
    ctx = &self;
    return (*callee_)(ctx, private_data_);
  }

  libatapp_c_on_all_module_inited_fn_t callee_;
  void *private_data_;
};

struct libatapp_c_on_cmd_option_functor {
  libatapp_c_on_cmd_option_functor(libatapp_c_context ctx, libatapp_c_on_cmd_option_fn_t fn, void *priv_data)
      : ctx_(ctx), callee_(fn), private_data_(priv_data) {}

  int operator()(atfw::util::cli::callback_param params) {
    if (nullptr == callee_) {
      return 0;
    }

    std::vector<const char *> buf_addrs;
    std::vector<uint64_t> buf_sz;
    buf_addrs.resize(params.get_params_number());
    buf_sz.resize(params.get_params_number());
    for (size_t i = 0; i < params.get_params_number(); ++i) {
      buf_addrs[i] = params[i]->to_cpp_string().c_str();
      buf_sz[i] = static_cast<uint64_t>(params[i]->to_cpp_string().size());
    }

    return (*callee_)(ctx_, &params, &buf_addrs[0], &buf_sz[0], static_cast<uint64_t>(params.get_params_number()),
                      private_data_);
  }

  libatapp_c_context ctx_;
  libatapp_c_on_cmd_option_fn_t callee_;
  void *private_data_;
};

class libatapp_c_on_module final : public ::atapp::module_impl {
 public:
  libatapp_c_on_module(const char *name) {
    name_ = name;

    on_init_ = nullptr;
    on_reload_ = nullptr;
    on_stop_ = nullptr;
    on_timeout_ = nullptr;
    on_cleanup_ = nullptr;
    on_tick_ = nullptr;
    on_init_private_data_ = nullptr;
    on_reload_private_data_ = nullptr;
    on_stop_private_data_ = nullptr;
    on_timeout_private_data_ = nullptr;
    on_tick_private_data_ = nullptr;
    on_cleanup_private_data_ = nullptr;
  }

  int init() override final {
    if (nullptr != on_init_) {
      libatapp_c_module mod;
      mod = this;
      return (*on_init_)(mod, on_init_private_data_);
    }
    return 0;
  };

  int reload() override final {
    if (nullptr != on_reload_) {
      libatapp_c_module mod;
      mod = this;
      return (*on_reload_)(mod, on_reload_private_data_);
    }
    return 0;
  }

  int stop() override final {
    if (nullptr != on_stop_) {
      libatapp_c_module mod;
      mod = this;
      return (*on_stop_)(mod, on_stop_private_data_);
    }
    return 0;
  }

  int timeout() override final {
    if (nullptr != on_timeout_) {
      libatapp_c_module mod;
      mod = this;
      return (*on_timeout_)(mod, on_timeout_private_data_);
    }
    return 0;
  }

  void cleanup() override final {
    if (nullptr != on_cleanup_) {
      libatapp_c_module mod;
      mod = this;
      (*on_cleanup_)(mod, on_cleanup_private_data_);
    }
  }

  const char *name() const override final { return name_.c_str(); }

  int tick() override final {
    if (nullptr != on_tick_) {
      libatapp_c_module mod;
      mod = this;
      return (*on_tick_)(mod, on_tick_private_data_);
    }

    return 0;
  }

  using ::atapp::module_impl::get_app;

 public:
  libatapp_c_module_on_init_fn_t on_init_;
  void *on_init_private_data_;
  libatapp_c_module_on_reload_fn_t on_reload_;
  void *on_reload_private_data_;
  libatapp_c_module_on_stop_fn_t on_stop_;
  void *on_stop_private_data_;
  libatapp_c_module_on_timeout_fn_t on_timeout_;
  void *on_timeout_private_data_;
  libatapp_c_module_on_cleanup_fn_t on_cleanup_;
  void *on_cleanup_private_data_;
  libatapp_c_module_on_tick_fn_t on_tick_;
  void *on_tick_private_data_;

 private:
  std::string name_;
};
}  // namespace detail

#define ATAPP_MODULE(x) ((::detail::libatapp_c_on_module *)(x))
#define ATAPP_MODULE_IS_NULL(x) (nullptr == (x))

#ifdef __cplusplus
extern "C" {
#endif

LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_msg_fn(libatapp_c_context context, libatapp_c_on_msg_fn_t fn,
                                                         void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  ATAPP_CONTEXT(context)->set_evt_on_forward_request(::detail::libatapp_c_on_msg_functor(fn, priv_data));
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_forward_response_fn(libatapp_c_context context,
                                                                      libatapp_c_on_send_fail_fn_t fn,
                                                                      void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  ATAPP_CONTEXT(context)->set_evt_on_forward_response(::detail::libatapp_c_on_send_fail_functor(fn, priv_data));
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_connected_fn(libatapp_c_context context,
                                                               libatapp_c_on_connected_fn_t fn, void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  ATAPP_CONTEXT(context)->set_evt_on_app_connected(::detail::libatapp_c_on_connected_functor(fn, priv_data));
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_disconnected_fn(libatapp_c_context context,
                                                                  libatapp_c_on_disconnected_fn_t fn, void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  ATAPP_CONTEXT(context)->set_evt_on_app_disconnected(::detail::libatapp_c_on_disconnected_functor(fn, priv_data));
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_all_module_inited_fn(libatapp_c_context context,
                                                                       libatapp_c_on_all_module_inited_fn_t fn,
                                                                       void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  ATAPP_CONTEXT(context)->set_evt_on_all_module_inited(
      ::detail::libatapp_c_on_all_module_inited_functor(fn, priv_data));
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_add_cmd(libatapp_c_context context, const char *cmd,
                                                   libatapp_c_on_cmd_option_fn_t fn, const char *help_msg,
                                                   void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  if (nullptr != help_msg && *help_msg) {
    ATAPP_CONTEXT(context)
        ->get_command_manager()
        ->bind_cmd(cmd, detail::libatapp_c_on_cmd_option_functor(context, fn, priv_data))
        ->set_help_msg(help_msg);
  } else {
    ATAPP_CONTEXT(context)->get_command_manager()->bind_cmd(
        cmd, detail::libatapp_c_on_cmd_option_functor(context, fn, priv_data));
  }
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_add_option(libatapp_c_context context, const char *opt,
                                                      libatapp_c_on_cmd_option_fn_t fn, const char *help_msg,
                                                      void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return;
  }

  if (nullptr != help_msg && *help_msg) {
    ATAPP_CONTEXT(context)
        ->get_option_manager()
        ->bind_cmd(opt, detail::libatapp_c_on_cmd_option_functor(context, fn, priv_data))
        ->set_help_msg(help_msg);
  } else {
    ATAPP_CONTEXT(context)->get_option_manager()->bind_cmd(
        opt, detail::libatapp_c_on_cmd_option_functor(context, fn, priv_data));
  }
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_custom_cmd_add_rsp(libatapp_c_custom_cmd_sender sender, const char *rsp,
                                                              uint64_t rsp_sz) {
  if (nullptr == sender) {
    return;
  }

  ::atapp::app::add_custom_command_rsp(*(atfw::util::cli::cmd_option_list *)sender, std::string(rsp, rsp_sz));
}

LIBATAPP_MACRO_API libatapp_c_context __cdecl libatapp_c_create() {
  libatapp_c_context ret;
  assert(sizeof(void *) == sizeof(libatapp_c_context));

  atapp::app *res = new (std::nothrow) atapp::app();
  ret = res;
  return ret;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_destroy(libatapp_c_context context) { delete ATAPP_CONTEXT(context); }

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_run(libatapp_c_context context, int32_t argc, const char **argv,
                                                  void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->run(uv_default_loop(), argc, argv, priv_data);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_init(libatapp_c_context context, int32_t argc, const char **argv,
                                                   void *priv_data) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->init(uv_default_loop(), argc, argv, priv_data);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_run_noblock(libatapp_c_context context, uint64_t max_event_count) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->run_noblock(max_event_count);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_reload(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->reload();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_stop(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->stop();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_tick(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->tick();
}

LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_get_id(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return 0;
  }

  return ATAPP_CONTEXT(context)->get_id();
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_get_app_version(libatapp_c_context context, const char **verbuf,
                                                           uint64_t *bufsz) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    if (nullptr != bufsz) {
      *bufsz = 0;
    }
    if (nullptr != verbuf) {
      *verbuf = nullptr;
    }

    return;
  }

  if (nullptr != bufsz) {
    *bufsz = static_cast<uint64_t>(ATAPP_CONTEXT(context)->get_app_version().size());
  }
  if (nullptr != verbuf) {
    *verbuf = ATAPP_CONTEXT(context)->get_app_version().c_str();
  }
}

LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_get_configure_size(libatapp_c_context context, const char *path) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return 0;
  }

  return static_cast<uint64_t>(ATAPP_CONTEXT(context)->get_configure_loader().get_node(path).size());
}

LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_get_configure(libatapp_c_context context, const char *path,
                                                             const char *out_buf[], uint64_t out_len[],
                                                             uint64_t arr_sz) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return 0;
  }

  atfw::util::config::ini_value &val = ATAPP_CONTEXT(context)->get_configure_loader().get_node(path);
  uint64_t ret = 0;
  for (ret = 0; ret < val.size() && ret < arr_sz; ++ret) {
    out_buf[ret] = val.as_cpp_string(static_cast<size_t>(ret)).c_str();
    out_len[ret] = static_cast<uint64_t>(val.as_cpp_string(static_cast<size_t>(ret)).size());
  }

  return ret;
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_inited(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return false;
  }

  return ATAPP_CONTEXT(context)->is_inited();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_running(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return false;
  }

  return ATAPP_CONTEXT(context)->is_running();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_closing(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return false;
  }

  return ATAPP_CONTEXT(context)->is_closing();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_closed(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return false;
  }

  return ATAPP_CONTEXT(context)->is_closed();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_stoping(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return 0;
  }

  return ATAPP_CONTEXT(context)->check_flag(atapp::app::flag_t::STOPING);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_timeout(libatapp_c_context context) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return 0;
  }

  return ATAPP_CONTEXT(context)->check_flag(atapp::app::flag_t::TIMEOUT);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_listen(libatapp_c_context context, const char *address) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->get_bus_node()->listen(address);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_connect(libatapp_c_context context, const char *address) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->get_bus_node()->connect(address);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_disconnect(libatapp_c_context context, uint64_t app_id) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->get_bus_node()->disconnect(app_id);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_send_data_msg(libatapp_c_context context, uint64_t app_id, int32_t type,
                                                            const void *buffer, uint64_t sz, int32_t require_rsp) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  return ATAPP_CONTEXT(context)->get_bus_node()->send_data(app_id, type, buffer, sz, 0 != require_rsp);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_send_custom_msg(libatapp_c_context context, uint64_t app_id,
                                                              const void *arr_buf[], uint64_t arr_size[],
                                                              uint64_t arr_count) {
  if (ATAPP_CONTEXT_IS_NULL(context)) {
    return EN_ATBUS_ERR_PARAMS;
  }

  std::vector<size_t> szs;
  szs.resize(arr_count);
  for (uint64_t i = 0; i < arr_count; ++i) {
    szs[i] = arr_size[i];
  }

  return ATAPP_CONTEXT(context)->get_bus_node()->send_custom_cmd(app_id, arr_buf, &szs[0], arr_count);
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_msg_get_type(libatapp_c_message msg) {
  if (ATAPP_MESSAGE_IS_NULL(msg)) {
    return 0;
  }

  return ATAPP_MESSAGE(msg)->type;
}

LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_msg_get_sequence(libatapp_c_message msg) {
  if (ATAPP_MESSAGE_IS_NULL(msg)) {
    return 0;
  }

  return ATAPP_MESSAGE(msg)->message_sequence;
}

LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_msg_get_forward_from(libatapp_c_message msg) {
  if (ATAPP_MESSAGE_IS_NULL(msg)) {
    return 0;
  }

  return ATAPP_SENDER(msg)->id;
}

LIBATAPP_MACRO_API libatapp_c_module __cdecl libatapp_c_module_create(libatapp_c_context context,
                                                                      const char *mod_name) {
  libatapp_c_module ret;
  ret = nullptr;
  if (ATAPP_CONTEXT_IS_NULL(context) || nullptr == mod_name) {
    return ret;
  }

  std::shared_ptr< ::detail::libatapp_c_on_module> res = std::make_shared< ::detail::libatapp_c_on_module>(mod_name);
  if (!res) {
    return ret;
  }
  ret = res.get();
  ATAPP_CONTEXT(context)->add_module(res);

  return ret;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_get_name(libatapp_c_module mod, const char **namebuf,
                                                           uint64_t *bufsz) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    if (nullptr != bufsz) {
      *bufsz = 0;
    }
    if (nullptr != namebuf) {
      *namebuf = nullptr;
    }
    return;
  }

  if (nullptr != bufsz) {
    *bufsz = static_cast<uint64_t>(strlen(ATAPP_MODULE(mod)->name()));
  }
  if (nullptr != namebuf) {
    *namebuf = ATAPP_MODULE(mod)->name();
  }
}

LIBATAPP_MACRO_API libatapp_c_context __cdecl libatapp_c_module_get_context(libatapp_c_module mod) {
  libatapp_c_context ret;
  ret = nullptr;

  if (ATAPP_MODULE_IS_NULL(mod)) {
    return ret;
  }

  ret = ATAPP_MODULE(mod)->get_app();
  return ret;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_init(libatapp_c_module mod, libatapp_c_module_on_init_fn_t fn,
                                                              void *priv_data) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    return;
  }

  ATAPP_MODULE(mod)->on_init_ = fn;
  ATAPP_MODULE(mod)->on_init_private_data_ = priv_data;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_reload(libatapp_c_module mod,
                                                                libatapp_c_module_on_reload_fn_t fn, void *priv_data) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    return;
  }

  ATAPP_MODULE(mod)->on_reload_ = fn;
  ATAPP_MODULE(mod)->on_reload_private_data_ = priv_data;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_stop(libatapp_c_module mod, libatapp_c_module_on_stop_fn_t fn,
                                                              void *priv_data) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    return;
  }

  ATAPP_MODULE(mod)->on_stop_ = fn;
  ATAPP_MODULE(mod)->on_stop_private_data_ = priv_data;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_timeout(libatapp_c_module mod,
                                                                 libatapp_c_module_on_timeout_fn_t fn,
                                                                 void *priv_data) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    return;
  }

  ATAPP_MODULE(mod)->on_timeout_ = fn;
  ATAPP_MODULE(mod)->on_timeout_private_data_ = priv_data;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_cleanup(libatapp_c_module mod,
                                                                 libatapp_c_module_on_cleanup_fn_t fn,
                                                                 void *priv_data) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    return;
  }

  ATAPP_MODULE(mod)->on_cleanup_ = fn;
  ATAPP_MODULE(mod)->on_cleanup_private_data_ = priv_data;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_tick(libatapp_c_module mod, libatapp_c_module_on_tick_fn_t fn,
                                                              void *priv_data) {
  if (ATAPP_MODULE_IS_NULL(mod)) {
    return;
  }

  ATAPP_MODULE(mod)->on_tick_ = fn;
  ATAPP_MODULE(mod)->on_tick_private_data_ = priv_data;
}

LIBATAPP_MACRO_API int64_t __cdecl libatapp_c_get_unix_timestamp() {
  return atfw::util::time::time_utility::get_sys_now();
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_log_write(uint32_t tag, uint32_t level, const char *level_name,
                                                     const char *file_path, const char *func_name, uint32_t line_number,
                                                     const char *content) {
  atfw::util::log::log_wrapper *log_cat = atfw::util::log::log_wrapper::mutable_log_cat(tag);
  if (nullptr == log_cat) {
    return;
  }

  if (!log_cat->check_level(static_cast<atfw::util::log::log_wrapper::level_t::type>(level))) {
    return;
  }

  log_cat->log(
      atfw::util::log::log_formatter::caller_info_t(static_cast<atfw::util::log::log_wrapper::level_t::type>(level),
                                                    level_name, file_path, line_number, func_name),
      "%s", content);
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_log_update() { atfw::util::log::log_wrapper::update(); }

LIBATAPP_MACRO_API uint32_t __cdecl libatapp_c_log_get_level(uint32_t tag) {
  atfw::util::log::log_wrapper *log_cat = atfw::util::log::log_wrapper::mutable_log_cat(tag);
  if (nullptr == log_cat) {
    return 0;
  }

  return log_cat->get_level();
}

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_log_check_level(uint32_t tag, uint32_t level) {
  atfw::util::log::log_wrapper *log_cat = atfw::util::log::log_wrapper::mutable_log_cat(tag);
  if (nullptr == log_cat) {
    return 0;
  }

  return log_cat->check_level(static_cast<atfw::util::log::log_wrapper::level_t::type>(level)) ? 1 : 0;
}

LIBATAPP_MACRO_API void __cdecl libatapp_c_log_set_project_directory(const char *project_dir, uint64_t dirsz) {
  atfw::util::log::log_formatter::set_project_directory(project_dir, static_cast<size_t>(dirsz));
}

#ifdef __cplusplus
}
#endif
