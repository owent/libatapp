// Copyright 2021 atframework
// Created by owent on 2021-07-20

#include <detail/libatbus_error.h>

#include <atframe/atapp.h>
#include <atframe/connectors/atapp_connector_loopback.h>

namespace atapp {

LIBATAPP_MACRO_API atapp_connector_loopback::atapp_connector_loopback(app &owner)
    : atapp_connector_impl(owner),
      pending_message_size_(0)
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
      ,
      pending_message_count_(0)
#endif
{
  register_protocol("loopback");
}

LIBATAPP_MACRO_API atapp_connector_loopback::~atapp_connector_loopback() { cleanup(); }

LIBATAPP_MACRO_API const char *atapp_connector_loopback::name() noexcept { return "atapp::connector.loopback"; }

LIBATAPP_MACRO_API uint32_t
atapp_connector_loopback::get_address_type(const atbus::channel::channel_address_t &) const {
  uint32_t ret = 0;
  ret |= address_type_t::EN_ACAT_DUPLEX;
  ret |= address_type_t::EN_ACAT_LOCAL_HOST;
  ret |= address_type_t::EN_ACAT_LOCAL_PROCESS;

  return ret;
}

LIBATAPP_MACRO_API int32_t atapp_connector_loopback::on_start_listen(const atbus::channel::channel_address_t &) {
  if (nullptr == get_owner()) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  return EN_ATAPP_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_loopback::on_start_connect(const etcd_discovery_node *,
                                                                      const atbus::channel::channel_address_t &,
                                                                      const atapp_connection_handle::ptr_t &handle) {
  if (nullptr == get_owner()) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  uintptr_t key = reinterpret_cast<uintptr_t>(handle.get());
  if (handle) {
    handles_[key] = handle;
    handle->set_ready();
    handle->set_private_data_u64(key);
  }

  return EN_ATAPP_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_loopback::on_close_connect(atapp_connection_handle &handle) {
  uintptr_t key = reinterpret_cast<uintptr_t>(&handle);
  // remove handle
  auto iter = handles_.find(key);
  if (iter != handles_.end()) {
    handles_.erase(iter);
  }
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_loopback::on_send_forward_request(
    atapp_connection_handle *handle, int32_t type, uint64_t *sequence, const void *data, size_t data_size,
    const atapp::protocol::atapp_metadata *metadata) {
  if (nullptr == get_owner() || nullptr == handle) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  if (nullptr == data || data_size <= 0) {
    return EN_ATBUS_ERR_SUCCESS;
  }

  uint64_t send_buffer_number = get_owner()->get_origin_configure().bus().send_buffer_number();
  uint64_t send_buffer_size = get_owner()->get_origin_configure().bus().send_buffer_size();
  if (send_buffer_number > 0 &&
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
      pending_message_count_ + 1 > send_buffer_number
#else
      pending_message_.size() + 1 > send_buffer_number
#endif
  ) {
    return EN_ATBUS_ERR_BUFF_LIMIT;
  }

  if (send_buffer_size > 0 && pending_message_size_ + data_size > send_buffer_size) {
    return EN_ATBUS_ERR_BUFF_LIMIT;
  }

  pending_message_.push_back(pending_message_t());
  pending_message_t &msg = pending_message_.back();
  msg.type = type;
  msg.message_sequence = nullptr == sequence ? 0 : *sequence;
  msg.data.resize(data_size);
  memcpy(&msg.data[0], data, data_size);
  if (nullptr != metadata) {
    msg.metadata.reset(new atapp::protocol::atapp_metadata());
    if (msg.metadata) {
      msg.metadata->CopyFrom(*metadata);
    }
  }

  pending_message_size_ += data_size;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  ++pending_message_count_;
#endif
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API void atapp_connector_loopback::on_receive_forward_response(
    atapp_connection_handle *handle, int32_t type, uint64_t msg_sequence, int32_t error_code, const void *data,
    size_t data_size, const atapp::protocol::atapp_metadata *metadata) {
  auto owner = get_owner();
  if (nullptr == owner) {
    return;
  }

  // notify app
  app::message_t msg;
  msg.data = data;
  msg.data_size = data_size;
  msg.metadata = metadata;
  msg.message_sequence = msg_sequence;
  msg.type = type;

  app::message_sender_t sender;
  sender.id = owner->get_id();
  sender.name = owner->get_app_name();
  if (nullptr != handle) {
    sender.remote = handle->get_endpoint();
  }
  if (nullptr == sender.remote) {
    if (0 != sender.id) {
      sender.remote = owner->get_endpoint(sender.id);
    } else {
      sender.remote = owner->get_endpoint(static_cast<std::string>(sender.name));
    }
  }

  get_owner()->trigger_event_on_forward_response(sender, msg, error_code);
}

LIBATAPP_MACRO_API void atapp_connector_loopback::on_discovery_event(etcd_discovery_action_t::type,
                                                                     const etcd_discovery_node::ptr_t &) {}

LIBATAPP_MACRO_API int32_t atapp_connector_loopback::process(
    const util::time::time_utility::raw_time_t &max_end_timepoint, int32_t max_loop_messages) {
  int32_t ret = 0;
  auto owner = get_owner();
  if (nullptr == owner) {
    return ret;
  }

  if (max_loop_messages <= 0) {
    max_loop_messages = 1000;
  }

  while (!pending_message_.empty()) {
    int32_t limit = max_loop_messages > 10 ? max_loop_messages / 10 : max_loop_messages;
    while (limit-- > 0 && !pending_message_.empty()) {
      pending_message_t &pending_msg = pending_message_.front();

      app::message_t msg;
      msg.data = pending_msg.data.data();
      msg.data_size = pending_msg.data.size();
      msg.metadata = pending_msg.metadata.get();
      msg.message_sequence = pending_msg.message_sequence;
      msg.type = pending_msg.type;

      app::message_sender_t sender;
      sender.id = owner->get_id();
      sender.name = owner->get_app_name();
      // endpoint maybe replaced in callback , so we need refind it every time
      if (0 == sender.id) {
        sender.remote = owner->get_endpoint(static_cast<std::string>(sender.name));
      } else {
        sender.remote = owner->get_endpoint(sender.id);
      }

      int res = owner->trigger_event_on_forward_request(sender, msg);
      if (res < 0) {
        FWLOGERROR("{} forward data {}(type={}, sequence={}) bytes failed, error code: {}", name(),
                   pending_msg.data.size(), pending_msg.type, pending_msg.message_sequence, res);
      } else {
        FWLOGDEBUG("{} forward data {}(type={}, sequence={}) bytes success, result code: {}", name(),
                   pending_msg.data.size(), pending_msg.type, pending_msg.message_sequence, res);
      }

      ++ret;
      UTIL_LIKELY_IF(pending_message_size_ >= pending_msg.data.size()) {
        pending_message_size_ -= pending_msg.data.size();
      }
      else {
        pending_message_size_ = 0;
      }
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
      if (pending_message_count_ > 0) {
        --pending_message_count_;
      }
#endif
      pending_message_.pop_front();
    }

    if (util::time::time_utility::sys_now() >= max_end_timepoint) {
      break;
    }
  }

  return ret;
}

}  // namespace atapp
