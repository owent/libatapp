// Copyright 2021 atframework
// Created by owent

#pragma once

#include "atframe/connectors/atapp_connector_impl.h"

namespace atbus {
class node;
class endpoint;
}  // namespace atbus

namespace atapp {

class atapp_connector_atbus : public atapp_connector_impl {
  UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connector_atbus)
  UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connector_atbus)

 public:
  LIBATAPP_MACRO_API atapp_connector_atbus(app &owner);
  LIBATAPP_MACRO_API virtual ~atapp_connector_atbus();
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

  using atapp_connector_impl::on_receive_forward_response;
  LIBATAPP_MACRO_API void on_receive_forward_response(uint64_t app_id, int32_t type, uint64_t msg_sequence,
                                                      int32_t error_code, const void *data, size_t data_size,
                                                      const atapp::protocol::atapp_metadata *metadata);

 private:
  friend class app;
  int on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res);
  int on_remove_endpoint(const atbus::node &n, atbus::endpoint *ep, int res);

 private:
  std::unordered_map<uint64_t, atapp_connection_handle::ptr_t> handles_;
};
}  // namespace atapp
