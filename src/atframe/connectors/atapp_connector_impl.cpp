// Copyright 2021 atframework
// Created by owent

#include "atframe/connectors/atapp_connector_impl.h"

#include <common/string_oprs.h>

#include <algorithm>

#include "atframe/atapp.h"
#include "atframe/connectors/atapp_endpoint.h"

namespace atapp {

LIBATAPP_MACRO_API_SYMBOL_HIDDEN void atapp_connector_bind_helper::unbind(atapp_connection_handle &handle,
                                                                          atapp_connector_impl &connect) {
  bool need_trigger = false;
  if (handle.connector_ == &connect) {
    handle.connector_ = nullptr;
    need_trigger = true;
  }
  connect.handles_.erase(&handle);

  if (need_trigger) {
    // It's safe to recall close after set handle.connector_ = nullptr
    handle.close();

    // child shoud call cleanup() to trigger on_close_connect event
    if (!connect.is_destroying_) {
      connect.on_close_connect(handle);
    }
  }
}

LIBATAPP_MACRO_API_SYMBOL_HIDDEN void atapp_connector_bind_helper::bind(atapp_connection_handle &handle,
                                                                        atapp_connector_impl &connect) {
  if (handle.connector_ == &connect || connect.is_destroying_) {
    return;
  }

  if (nullptr != handle.connector_) {
    unbind(handle, *handle.connector_);
  }

  handle.connector_ = &connect;
  connect.handles_.insert(&handle);
}

LIBATAPP_MACRO_API_SYMBOL_HIDDEN void atapp_endpoint_bind_helper::unbind(atapp_connection_handle &handle,
                                                                         atapp_endpoint &endpoint) {
  if (endpoint.refer_connections_.erase(&handle) > 0) {
    if (endpoint.refer_connections_.empty()) {
      if (nullptr != endpoint.owner_) {
        endpoint.add_waker(endpoint.owner_->get_last_tick_time());
      }
    }
  }
  if (handle.endpiont_ == &endpoint) {
    handle.endpiont_ = nullptr;

    // It's safe to recall close after set handle.endpiont_ = nullptr
    handle.close();
  }
}

LIBATAPP_MACRO_API_SYMBOL_HIDDEN void atapp_endpoint_bind_helper::bind(atapp_connection_handle &handle,
                                                                       atapp_endpoint &endpoint) {
  if (handle.endpiont_ == &endpoint) {
    return;
  }

  if (nullptr != handle.endpiont_) {
    unbind(handle, *handle.endpiont_);
  }

  handle.endpiont_ = &endpoint;
  endpoint.refer_connections_.insert(&handle);

  if (handle.is_ready()) {
    app *owner = endpoint.get_owner();
    if (nullptr != owner) {
      endpoint.add_waker(owner->get_last_tick_time());
    }
  }
}

LIBATAPP_MACRO_API atapp_connection_handle::atapp_connection_handle()
    : flags_(0), connector_(nullptr), endpiont_(nullptr) {
  private_data_ptr_ = nullptr;
  private_data_u64_ = 0;
  private_data_uptr_ = 0;

  FWLOGDEBUG("atapp handle {} created", reinterpret_cast<const void *>(this));
}

LIBATAPP_MACRO_API atapp_connection_handle::~atapp_connection_handle() {
  FWLOGDEBUG("atapp handle {} destroying", reinterpret_cast<const void *>(this));

  close();
  if (on_destroy_fn_) {
    on_destroy_fn_(*this);
  }
}

LIBATAPP_MACRO_API void atapp_connection_handle::close() {
  flags_ |= flags_t::EN_ACH_CLOSING;
  flags_ &= ~static_cast<uint32_t>(flags_t::EN_ACH_READY);

  // Maybe recursive call, check connector_ first
  if (nullptr != connector_) {
    atapp_connector_bind_helper::unbind(*this, *connector_);
  }

  // Maybe recursive call, check endpiont_ first
  if (nullptr != endpiont_) {
    atapp_endpoint_bind_helper::unbind(*this, *endpiont_);
  }
}

LIBATAPP_MACRO_API bool atapp_connection_handle::is_closing() const noexcept {
  return 0 != (flags_ & flags_t::EN_ACH_CLOSING);
}

LIBATAPP_MACRO_API void atapp_connection_handle::set_ready() noexcept {
  flags_ |= flags_t::EN_ACH_READY;
  flags_ &= ~static_cast<uint32_t>(flags_t::EN_ACH_CLOSING);

  // reactive endpoint and call retry_pending_messages()
  if (nullptr != endpiont_ && nullptr != connector_) {
    app *owner = connector_->get_owner();
    if (nullptr != owner) {
      endpiont_->add_waker(owner->get_last_tick_time());
    }
  }
}

LIBATAPP_MACRO_API bool atapp_connection_handle::is_ready() const noexcept {
  return 0 != (flags_ & flags_t::EN_ACH_READY);
}

LIBATAPP_MACRO_API void atapp_connection_handle::set_on_destroy(on_destroy_fn_t fn) { on_destroy_fn_ = fn; }

LIBATAPP_MACRO_API const atapp_connection_handle::on_destroy_fn_t &atapp_connection_handle::get_on_destroy() const {
  return on_destroy_fn_;
}

LIBATAPP_MACRO_API void atapp_connection_handle::reset_on_destroy() {
  on_destroy_fn_t empty;
  on_destroy_fn_.swap(empty);
}

LIBATAPP_MACRO_API atapp_connector_impl::atapp_connector_impl(app &owner) : owner_(&owner), is_destroying_(false) {}

LIBATAPP_MACRO_API atapp_connector_impl::~atapp_connector_impl() {
  is_destroying_ = true;
  cleanup();
}

LIBATAPP_MACRO_API const char *atapp_connector_impl::name() noexcept {
  if (auto_demangled_name_) {
    return auto_demangled_name_->get();
  }

#if defined(LIBATFRAME_UTILS_ENABLE_RTTI) && LIBATFRAME_UTILS_ENABLE_RTTI
  auto_demangled_name_.reset(new util::scoped_demangled_name(typeid(*this).name()));
  if (auto_demangled_name_) {
    return auto_demangled_name_->get();
  } else {
    return "atapp::connector demangle symbol failed";
  }
#else
  return "atapp::connector RTTI Unavailable";
#endif
}

LIBATAPP_MACRO_API void atapp_connector_impl::register_protocol(const std::string &protocol_name) {
  std::string name = protocol_name;
  std::transform(name.begin(), name.end(), name.begin(), ::util::string::tolower<char>);
  support_protocols_.insert(name);
}

LIBATAPP_MACRO_API void atapp_connector_impl::cleanup() {
  while (!handles_.empty()) {
    handle_set_t handles;
    handles.swap(handles_);

    for (handle_set_t::const_iterator iter = handles.begin(); iter != handles.end(); ++iter) {
      if (*iter) {
        atapp_connector_bind_helper::unbind(**iter, *this);
      }
    }
  }
}

LIBATAPP_MACRO_API bool atapp_connector_impl::support_loopback() const noexcept { return false; }

LIBATAPP_MACRO_API int32_t atapp_connector_impl::on_start_listen(const atbus::channel::channel_address_t &) {
  return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
}

LIBATAPP_MACRO_API int32_t atapp_connector_impl::on_start_connect(const etcd_discovery_node *,
                                                                  const atbus::channel::channel_address_t &,
                                                                  const atapp_connection_handle::ptr_t &) {
  return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
}

LIBATAPP_MACRO_API int32_t atapp_connector_impl::on_close_connect(atapp_connection_handle &) {
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_impl::on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                                         uint64_t *sequence, const void *,
                                                                         size_t data_size,
                                                                         const atapp::protocol::atapp_metadata *) {
  FWLOGERROR("{} forward data {}(type={}, sequence={}) bytes using handle {} failed, not support", name(), data_size,
             type, (sequence == nullptr ? 0 : *sequence), reinterpret_cast<const void *>(handle));
  return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
}

LIBATAPP_MACRO_API void atapp_connector_impl::on_receive_forward_response(
    atapp_connection_handle *handle, int32_t type, uint64_t sequence, int32_t error_code, const void *data,
    size_t data_size, const atapp::protocol::atapp_metadata *metadata) {
  if (nullptr == get_owner()) {
    return;
  }

  // notify app
  app::message_t msg;
  msg.data = data;
  msg.data_size = data_size;
  msg.metadata = metadata;
  msg.message_sequence = sequence;
  msg.type = type;

  app::message_sender_t sender;
  if (nullptr != handle) {
    sender.remote = handle->get_endpoint();
  }
  if (nullptr != sender.remote) {
    sender.id = sender.remote->get_id();
    sender.name = sender.remote->get_name();
  }

  get_owner()->trigger_event_on_forward_response(sender, msg, error_code);
}

LIBATAPP_MACRO_API void atapp_connector_impl::on_discovery_event(etcd_discovery_action_t::type,
                                                                 const etcd_discovery_node::ptr_t &) {}

LIBATAPP_MACRO_API const atapp_connector_impl::protocol_set_t &atapp_connector_impl::get_support_protocols()
    const noexcept {
  return support_protocols_;
}
}  // namespace atapp
