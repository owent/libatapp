// Copyright 2021 atframework
// Created by owent

#include <detail/libatbus_error.h>

#include <atframe/atapp.h>

#include <atframe/connectors/atapp_connector_impl.h>
#include <atframe/connectors/atapp_endpoint.h>

#include <limits>

#ifdef max
#  undef max
#endif

namespace atapp {

void atapp_endpoint::internal_accessor::close(atapp_endpoint &endpoint) { endpoint.reset(); }

LIBATAPP_MACRO_API atapp_endpoint::atapp_endpoint(app &owner, construct_helper_t &)
    : closing_(false),
      owner_(&owner),
      pending_message_size_(0)
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
      ,
      pending_message_count_(0)
#endif
{
  nearest_waker_ = std::chrono::system_clock::from_time_t(0);
}

LIBATAPP_MACRO_API atapp_endpoint::ptr_t atapp_endpoint::create(app &owner) {
  construct_helper_t helper;
  ptr_t ret = std::make_shared<atapp_endpoint>(owner, helper);
  if (ret) {
    ret->watcher_ = ret;

    FWLOGINFO("create atapp endpoint {}", reinterpret_cast<const void *>(ret.get()));
  }
  return ret;
}

LIBATAPP_MACRO_API atapp_endpoint::~atapp_endpoint() {
  FWLOGINFO("destroy atapp endpoint  {:#x}({}) - {}", get_id(), get_name(), reinterpret_cast<const void *>(this));
  reset();
}

void atapp_endpoint::reset() {
  if (closing_) {
    return;
  }
  closing_ = true;

  cancel_pending_messages();

  handle_set_t handles;
  handles.swap(refer_connections_);

  for (handle_set_t::const_iterator iter = handles.begin(); iter != handles.end(); ++iter) {
    if (*iter) {
      atapp_endpoint_bind_helper::unbind(**iter, *this);
    }
  }

  closing_ = false;
}

LIBATAPP_MACRO_API void atapp_endpoint::add_connection_handle(atapp_connection_handle &handle) {
  if (closing_) {
    return;
  }

  atapp_endpoint_bind_helper::bind(handle, *this);
}

LIBATAPP_MACRO_API void atapp_endpoint::remove_connection_handle(atapp_connection_handle &handle) {
  if (closing_) {
    return;
  }

  atapp_endpoint_bind_helper::unbind(handle, *this);
}

LIBATAPP_MACRO_API atapp_connection_handle *atapp_endpoint::get_ready_connection_handle() const noexcept {
  for (handle_set_t::const_iterator iter = refer_connections_.begin(); iter != refer_connections_.end(); ++iter) {
    if (*iter && (*iter)->is_ready()) {
      return *iter;
    }
  }

  return nullptr;
}

LIBATAPP_MACRO_API uint64_t atapp_endpoint::get_id() const noexcept {
  if (!discovery_) {
    return 0;
  }

  return discovery_->get_discovery_info().id();
}

LIBATAPP_MACRO_API const std::string &atapp_endpoint::get_name() const noexcept {
  if (!discovery_) {
    return atapp::protocol::atapp_discovery::default_instance().name();
  }

  return discovery_->get_discovery_info().name();
}

LIBATAPP_MACRO_API const etcd_discovery_node::ptr_t &atapp_endpoint::get_discovery() const noexcept {
  return discovery_;
}

LIBATAPP_MACRO_API void atapp_endpoint::update_discovery(const etcd_discovery_node::ptr_t &discovery) noexcept {
  if (discovery_ == discovery) {
    return;
  }

  discovery_ = discovery;

  if (discovery) {
    FWLOGINFO("update atapp endpoint {} with {}({})", reinterpret_cast<const void *>(this),
              discovery->get_discovery_info().id(), discovery->get_discovery_info().name());
  }
}

LIBATAPP_MACRO_API int32_t atapp_endpoint::push_forward_message(int32_t type, uint64_t &msg_sequence, const void *data,
                                                                size_t data_size,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  // Closing
  if (closing_ || nullptr == owner_) {
    do {
      atapp_connection_handle *handle = get_ready_connection_handle();
      atapp_connector_impl *connector = nullptr;
      if (nullptr != handle) {
        connector = handle->get_connector();
      }

      trigger_on_receive_forward_response(connector, handle, type, msg_sequence, EN_ATBUS_ERR_CLOSING, data, data_size,
                                          metadata);
    } while (false);
    return EN_ATBUS_ERR_CLOSING;
  }

  if (nullptr == data || 0 == data_size) {
    return EN_ATBUS_ERR_SUCCESS;
  }

  // Has handle
  do {
    if (!pending_message_.empty()) {
      break;
    }

    atapp_connection_handle *handle = get_ready_connection_handle();
    if (nullptr == handle) {
      break;
    }

    atapp_connector_impl *connector = handle->get_connector();
    if (nullptr == connector) {
      break;
    }

    int32_t ret = connector->on_send_forward_request(handle, type, &msg_sequence, data, data_size, metadata);
    if (0 != ret) {
      trigger_on_receive_forward_response(connector, handle, type, msg_sequence, ret, data, data_size, metadata);
    }

    return ret;
  } while (false);

  // Failed to add to pending
  int32_t failed_error_code = 0;
  if (nullptr != owner_) {
    uint64_t send_buffer_number = owner_->get_origin_configure().bus().send_buffer_number();
    uint64_t send_buffer_size = owner_->get_origin_configure().bus().send_buffer_size();
    if (send_buffer_number > 0 &&
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
        pending_message_count_ + 1 > send_buffer_number
#else
        pending_message_.size() + 1 > send_buffer_number
#endif
    ) {
      failed_error_code = EN_ATBUS_ERR_BUFF_LIMIT;
    }

    if (send_buffer_size > 0 && pending_message_size_ + data_size > send_buffer_size) {
      failed_error_code = EN_ATBUS_ERR_BUFF_LIMIT;
    }
  }

  if (failed_error_code != 0) {
    atapp_connection_handle *handle = get_ready_connection_handle();
    atapp_connector_impl *connector = nullptr;
    if (nullptr != handle) {
      connector = handle->get_connector();
    }

    trigger_on_receive_forward_response(connector, handle, type, msg_sequence, failed_error_code, data, data_size,
                                        metadata);
    return failed_error_code;
  }

  // Success to add to pending
  pending_message_.push_back(pending_message_t());
  pending_message_t &msg = pending_message_.back();
  msg.type = type;
  msg.message_sequence = msg_sequence;
  msg.data.resize(data_size);
  msg.expired_timepoint = owner_->get_last_tick_time();
  msg.expired_timepoint += owner_->get_configure_message_timeout();
  memcpy(&msg.data[0], data, data_size);
  if (nullptr != metadata) {
    msg.metadata.reset(new atapp::protocol::atapp_metadata());
    if (msg.metadata) {
      *msg.metadata = *metadata;
    }
  }

  pending_message_size_ += data_size;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  ++pending_message_count_;
#endif

  add_waker(msg.expired_timepoint);
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_endpoint::retry_pending_messages(const util::time::time_utility::raw_time_t &tick_time,
                                                                  int32_t max_count) {
  // Including equal
  if (nearest_waker_ <= tick_time) {
    nearest_waker_ = std::chrono::system_clock::from_time_t(0);
  }

  int ret = 0;
  if (pending_message_.empty()) {
    return ret;
  }

  if (max_count <= 0) {
    max_count = std::numeric_limits<int32_t>::max();
  }

  atapp_connection_handle *handle = get_ready_connection_handle();
  atapp_connector_impl *connector = nullptr;
  if (nullptr != handle) {
    connector = handle->get_connector();
    FWLOGDEBUG("Retry send pending message to {:#x}({}) with connector {} and handle {}", get_id(), get_name(),
               reinterpret_cast<const void *>(connector), reinterpret_cast<const void *>(handle));
  }

  while (!pending_message_.empty()) {
    pending_message_t &msg = pending_message_.front();

    int res = EN_ATBUS_ERR_NODE_TIMEOUT;
    // Support to send data after reconnected
    if (max_count > 0 && nullptr != handle && nullptr != connector) {
      --max_count;
      res = connector->on_send_forward_request(handle, msg.type, &msg.message_sequence,
                                               reinterpret_cast<const void *>(msg.data.data()), msg.data.size(),
                                               msg.metadata.get());
    } else if (msg.expired_timepoint > tick_time || max_count <= 0) {
      break;
    }

    if (0 != res) {
      trigger_on_receive_forward_response(connector, handle, msg.type, msg.message_sequence, res,
                                          reinterpret_cast<const void *>(msg.data.data()), msg.data.size(),
                                          msg.metadata.get());
    }

    ++ret;

    UTIL_LIKELY_IF (pending_message_size_ >= msg.data.size()) {
      pending_message_size_ -= msg.data.size();
    } else {
      pending_message_size_ = 0;
    }
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
    if (pending_message_count_ > 0) {
      --pending_message_count_;
    }
#endif
    pending_message_.pop_front();
  }

  if (pending_message_.empty()) {
    pending_message_size_ = 0;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
    pending_message_count_ = 0;
#endif
  } else if (nullptr != owner_) {
    add_waker(pending_message_.front().expired_timepoint);
  }

  return ret;
}

LIBATAPP_MACRO_API void atapp_endpoint::add_waker(util::time::time_utility::raw_time_t wakeup_time) {
  if (wakeup_time < nearest_waker_ || std::chrono::system_clock::to_time_t(nearest_waker_) == 0) {
    if (nullptr != owner_) {
      if (owner_->add_endpoint_waker(wakeup_time, watcher_, nearest_waker_)) {
        nearest_waker_ = wakeup_time;
        FWLOGDEBUG("atapp {:#x}({}) update waker for {}({:#x}, {}) to {}us later", owner_->get_app_id(),
                   owner_->get_app_name(), reinterpret_cast<const void *>(this), get_id(), get_name(),
                   std::max<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         nearest_waker_ - util::time::time_utility::sys_now())
                                         .count(),
                                     0));
      }
    }
  }
}

LIBATAPP_MACRO_API size_t atapp_endpoint::get_pending_message_count() const noexcept {
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  return pending_message_count_;
#else
  return pending_message_.size();
#endif
}

LIBATAPP_MACRO_API size_t atapp_endpoint::get_pending_message_size() const noexcept { return pending_message_size_; }

void atapp_endpoint::cancel_pending_messages() {
  atapp_connection_handle *handle = get_ready_connection_handle();
  atapp_connector_impl *connector = nullptr;
  if (nullptr != handle) {
    connector = handle->get_connector();
  }

  if (nullptr == connector) {
    return;
  }

  FWLOGDEBUG("atapp {:#x}({}) cancel pending message to {}({:#x}, {}) with connector {} and handle {}",
             owner_->get_app_id(), owner_->get_app_name(), reinterpret_cast<const void *>(this), get_id(), get_name(),
             reinterpret_cast<const void *>(connector), reinterpret_cast<const void *>(handle));

  while (!pending_message_.empty()) {
    const pending_message_t &msg = pending_message_.front();
    trigger_on_receive_forward_response(connector, handle, msg.type, msg.message_sequence, EN_ATBUS_ERR_CLOSING,
                                        reinterpret_cast<const void *>(msg.data.data()), msg.data.size(),
                                        msg.metadata.get());

    UTIL_LIKELY_IF (pending_message_size_ >= msg.data.size()) {
      pending_message_size_ -= msg.data.size();
    } else {
      pending_message_size_ = 0;
    }
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
    if (pending_message_count_ > 0) {
      --pending_message_count_;
    }
#endif
    pending_message_.pop_front();
  }

  pending_message_size_ = 0;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  pending_message_count_ = 0;
#endif
}

void atapp_endpoint::trigger_on_receive_forward_response(atapp_connector_impl *connector,
                                                         atapp_connection_handle *handle, int32_t type,
                                                         uint64_t sequence, int32_t error_code, const void *data,
                                                         size_t data_size,
                                                         const atapp::protocol::atapp_metadata *metadata) {
  if (nullptr != connector && nullptr != handle) {
    connector->on_receive_forward_response(handle, type, sequence, error_code, data, data_size, metadata);
    return;
  }

  atapp::app *app = get_owner();
  if (nullptr == app) {
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

  app->trigger_event_on_forward_response(sender, msg, error_code);
}

}  // namespace atapp
