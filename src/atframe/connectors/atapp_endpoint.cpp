// Copyright 2026 atframework
//
// Created by owent

#include <detail/libatbus_error.h>

#include <atframe/atapp.h>

#include <atframe/connectors/atapp_connector_impl.h>
#include <atframe/connectors/atapp_endpoint.h>

#include <limits>
#include <memory>
#include "atframe/atapp_common_types.h"

#ifdef max
#  undef max
#endif

LIBATAPP_MACRO_NAMESPACE_BEGIN

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

  gc_timepoint_ = owner.get_last_tick_time();
  const auto &endpoint_gc_timeout = owner.get_origin_configure().timer().endpoint_gc_timeout();
  if (endpoint_gc_timeout.seconds() < 0) {
    gc_timepoint_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::seconds(60));
  } else {
    gc_timepoint_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::seconds(endpoint_gc_timeout.seconds()));
    gc_timepoint_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::nanoseconds(endpoint_gc_timeout.nanos()));
  }
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
    if (*iter != nullptr) {
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
    if (*iter != nullptr && (*iter)->is_ready()) {
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

  if (!discovery) {
    return;
  }

  FWLOGINFO("update atapp endpoint {} with {}({})", reinterpret_cast<const void *>(this),
            discovery->get_discovery_info().id(), discovery->get_discovery_info().name());

  if (owner_->get_bus_node() && discovery->get_discovery_info().id() != 0) {
    auto *bus_ep = owner_->get_bus_node()->get_endpoint(discovery->get_discovery_info().id());
    if (bus_ep != nullptr) {
      std::unordered_map<std::string, std::string> labels;
      std::vector<atbus::node::gateway_t> gateways;
      const auto &metadata = discovery->get_discovery_info().metadata();
      std::unordered_set<std::string> inherited_labels_set;
      inherited_labels_set.reserve(static_cast<size_t>(owner_->get_origin_configure().bus().inherited_labels().size()));
      for (const auto &label_k : owner_->get_origin_configure().bus().inherited_labels()) {
        inherited_labels_set.insert(label_k);
        auto iter = metadata.labels().find(label_k);
        if (iter != metadata.labels().end() && !iter->second.empty()) {
          labels[label_k] = iter->second;
        }
      }

      gateways.reserve(static_cast<size_t>(discovery->get_discovery_info().gateways_size()));
      for (const auto &gateway : discovery->get_discovery_info().gateways()) {
        if (gateway.address().empty()) {
          continue;
        }
        gateways.push_back(atbus::node::gateway_t());
        auto &gw = gateways.back();

        gw.address = gateway.address();
        gw.match_scope = gateway.match_scope();
        if (gateway.match_namespaces_size() > 0) {
          gw.match_namespaces.reserve(static_cast<size_t>(gateway.match_namespaces_size()));
          for (const auto &ns : gateway.match_namespaces()) {
            gw.match_namespaces.insert(ns);
          }
        }
        if (gateway.match_hosts_size() > 0) {
          gw.match_hosts.reserve(static_cast<size_t>(gateway.match_hosts_size()));
          for (const auto &host : gateway.match_hosts()) {
            gw.match_hosts.insert(host);
          }
        }
        if (gateway.match_labels_size() > 0) {
          // 与 apply_atbus_configure 保持一致: 只按继承标签过滤, 对端只会用继承标签来匹配
          gw.match_labels.reserve(
              (std::min)(static_cast<size_t>(gateway.match_labels_size()), inherited_labels_set.size()));
          for (const auto &label_kv : gateway.match_labels()) {
            if (inherited_labels_set.find(label_kv.first) != inherited_labels_set.end() && !label_kv.second.empty()) {
              gw.match_labels.emplace(label_kv.first, label_kv.second);
            }
          }
        }
      }

      if (gateways.empty()) {
        // 未配置 gateway 的对端注册时通告的是 listen 地址, 这里按同样的规则合成匹配条件
        gateways.reserve(static_cast<size_t>(discovery->get_discovery_info().listen_size()));
        for (const auto &listen_address : discovery->get_discovery_info().listen()) {
          if (listen_address.empty()) {
            continue;
          }
          gateways.push_back(atbus::node::gateway_t());
          auto &gw = gateways.back();

          gw.address = listen_address;
          gw.match_scope = metadata.scope();
          if (!metadata.namespace_name().empty()) {
            gw.match_namespaces.insert(metadata.namespace_name());
          }
        }
      }

      bus_ep->reload(metadata.scope(), metadata.namespace_name(), labels,
                     gsl::span<const atbus::node::gateway_t>(gateways.data(), gateways.size()));
    }
  }
}

LIBATAPP_MACRO_API int32_t atapp_endpoint::push_forward_message(int32_t type, uint64_t &msg_sequence,
                                                                gsl::span<const unsigned char> data,
                                                                const atapp::protocol::atapp_metadata *metadata) {
  // Closing
  app_id_t self_app_id = 0;
  if (owner_ != nullptr) {
    self_app_id = owner_->get_id();
  }
  if (closing_ || nullptr == owner_) {
    do {
      atapp_connection_handle *handle = get_ready_connection_handle();
      atapp_connector_impl *connector = nullptr;
      if (nullptr != handle) {
        connector = handle->get_connector();
      }

      trigger_on_receive_forward_response(self_app_id, connector, handle, type, msg_sequence, EN_ATBUS_ERR_CLOSING,
                                          data, metadata);
    } while (false);
    return EN_ATBUS_ERR_CLOSING;
  }

  if (data.empty()) {
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

    int32_t ret = connector->on_send_forward_request(handle, type, &msg_sequence, data, metadata);
    if (0 != ret) {
      // 连接错误直接走重试流程
      if (EN_ATBUS_ERR_ATNODE_NO_CONNECTION == ret || EN_ATBUS_ERR_ATNODE_INVALID_ID == ret) {
        break;
      }
      trigger_on_receive_forward_response(self_app_id, connector, handle, type, msg_sequence, ret, data, metadata);
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

    if (send_buffer_size > 0 && pending_message_size_ + data.size() > send_buffer_size) {
      failed_error_code = EN_ATBUS_ERR_BUFF_LIMIT;
    }
  }

  if (failed_error_code != 0) {
    atapp_connection_handle *handle = get_ready_connection_handle();
    atapp_connector_impl *connector = nullptr;
    if (nullptr != handle) {
      connector = handle->get_connector();
    }

    trigger_on_receive_forward_response(self_app_id, connector, handle, type, msg_sequence, failed_error_code, data,
                                        metadata);
    return failed_error_code;
  }

  // Success to add to pending
  pending_message_.emplace_back();
  pending_message_t &msg = pending_message_.back();
  msg.type = type;
  msg.message_sequence = msg_sequence;
  msg.data.resize(data.size());
  msg.expired_timepoint = owner_->get_last_tick_time();
  msg.expired_timepoint += owner_->get_configure_message_timeout();
  memcpy(msg.data.data(), data.data(), data.size());
  if (nullptr != metadata) {
    msg.metadata = gsl::make_unique<atapp::protocol::atapp_metadata>();
    if (msg.metadata) {
      *msg.metadata = *metadata;
    }
  }

  pending_message_size_ += data.size();
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  ++pending_message_count_;
#endif

  add_waker(msg.expired_timepoint);
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t
atapp_endpoint::retry_pending_messages(const atfw::util::time::time_utility::raw_time_t &tick_time, int32_t max_count) {
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

  app_id_t self_app_id = 0;
  if (owner_ != nullptr) {
    self_app_id = owner_->get_id();
  }

  while (!pending_message_.empty()) {
    pending_message_t &msg = pending_message_.front();

    int res = EN_ATBUS_ERR_NODE_TIMEOUT;
    // Support to send data after reconnected
    if (max_count > 0 && nullptr != handle && nullptr != connector) {
      --max_count;
      res = connector->on_send_forward_request(handle, msg.type, &msg.message_sequence,
                                               gsl::span<const unsigned char>(msg.data.data(), msg.data.size()),
                                               msg.metadata.get());
    } else if (msg.expired_timepoint > tick_time || max_count <= 0) {
      break;
    }

    if (0 != res) {
      trigger_on_receive_forward_response(self_app_id, connector, handle, msg.type, msg.message_sequence, res,
                                          gsl::span<const unsigned char>(msg.data.data(), msg.data.size()),
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

LIBATAPP_MACRO_API void atapp_endpoint::add_waker(atfw::util::time::time_utility::raw_time_t wakeup_time) {
  if (wakeup_time < nearest_waker_ || std::chrono::system_clock::to_time_t(nearest_waker_) == 0) {
    if (nullptr != owner_) {
      if (owner_->add_endpoint_waker(wakeup_time, watcher_, nearest_waker_)) {
        nearest_waker_ = wakeup_time;
        FWLOGDEBUG("atapp {:#x}({}) update waker for {}({:#x}, {}) to {}us later", owner_->get_app_id(),
                   owner_->get_app_name(), reinterpret_cast<const void *>(this), get_id(), get_name(),
                   std::max<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(nearest_waker_ -
                                                                                           get_owner()->get_sys_now())
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
    trigger_on_receive_forward_response(
        owner_->get_app_id(), connector, handle, msg.type, msg.message_sequence, EN_ATBUS_ERR_CLOSING,
        gsl::span<const unsigned char>(msg.data.data(), msg.data.size()), msg.metadata.get());

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

LIBATAPP_MACRO_API atfw::util::time::time_utility::raw_time_t atapp_endpoint::get_gc_timepoint() const noexcept {
  return gc_timepoint_;
}

LIBATAPP_MACRO_API atfw::util::time::time_utility::raw_time_t atapp_endpoint::get_next_pending_message_timeout()
    const noexcept {
  if (pending_message_.empty()) {
    return std::chrono::system_clock::from_time_t(0);
  }

  return pending_message_.front().expired_timepoint;
}

void atapp_endpoint::trigger_on_receive_forward_response(app_id_t direct_source_id, atapp_connector_impl *connector,
                                                         atapp_connection_handle *handle, int32_t type,
                                                         uint64_t sequence, int32_t error_code,
                                                         gsl::span<const unsigned char> data,
                                                         const atapp::protocol::atapp_metadata *metadata) {
  if (nullptr != connector && nullptr != handle) {
    connector->on_receive_forward_response(direct_source_id, handle, type, sequence, error_code, data, metadata);
    return;
  }

  atframework::atapp::app *app = get_owner();
  if (nullptr == app) {
    return;
  }

  // notify app
  app::message_t msg;
  msg.data = data;
  msg.metadata = metadata;
  msg.message_sequence = sequence;
  msg.type = type;

  app::message_sender_t sender;
  sender.direct_source_id = direct_source_id;
  if (nullptr != handle) {
    sender.remote = handle->get_endpoint();
  }
  if (nullptr != sender.remote) {
    sender.id = sender.remote->get_id();
    sender.name = sender.remote->get_name();
  }

  app->trigger_event_on_forward_response(sender, msg, error_code);
}

LIBATAPP_MACRO_NAMESPACE_END
