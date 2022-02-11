// Copyright 2021 atframework
// Created by owent

#pragma once

#include <stdint.h>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "atframe/connectors/atapp_connector_impl.h"

namespace atapp {

class atapp_connector_loopback : public atapp_connector_impl {
  UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connector_loopback)
  UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connector_loopback)

 public:
  LIBATAPP_MACRO_API atapp_connector_loopback(app &owner);
  LIBATAPP_MACRO_API virtual ~atapp_connector_loopback();
  LIBATAPP_MACRO_API const char *name() noexcept override;
  LIBATAPP_MACRO_API uint32_t get_address_type(const atbus::channel::channel_address_t &addr) const override;
  LIBATAPP_MACRO_API int32_t on_start_listen(const atbus::channel::channel_address_t &addr) override;
  LIBATAPP_MACRO_API int32_t on_start_connect(const etcd_discovery_node *discovery,
                                              const atbus::channel::channel_address_t &addr,
                                              const atapp_connection_handle::ptr_t &handle) override;
  LIBATAPP_MACRO_API int32_t on_close_connect(atapp_connection_handle &handle) override;
  LIBATAPP_MACRO_API int32_t on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                     uint64_t *msg_sequence, const void *data, size_t data_size,
                                                     const atapp::protocol::atapp_metadata *metadata) override;

  LIBATAPP_MACRO_API void on_discovery_event(etcd_discovery_action_t::type,
                                             const etcd_discovery_node::ptr_t &) override;

  LIBATAPP_MACRO_API void on_receive_forward_response(atapp_connection_handle *handle, int32_t type,
                                                      uint64_t msg_sequence, int32_t error_code, const void *data,
                                                      size_t data_size,
                                                      const atapp::protocol::atapp_metadata *metadata) override;

  LIBATAPP_MACRO_API int32_t process(const util::time::time_utility::raw_time_t &max_end_timepoint,
                                     int32_t max_loop_messages);

 private:
  std::unordered_map<uintptr_t, atapp_connection_handle::ptr_t> handles_;
  struct pending_message_t {
    int32_t type;
    uint64_t message_sequence;
    std::vector<unsigned char> data;
    std::unique_ptr<atapp::protocol::atapp_metadata> metadata;
  };

  std::list<pending_message_t> pending_message_;
  size_t pending_message_size_;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
  size_t pending_message_count_;
#endif
};
}  // namespace atapp
