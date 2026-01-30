// Copyright 2021 atframework
// Created by owent

#pragma once

#include <time/time_utility.h>

#include <atbus_topology.h>
#include <detail/libatbus_config.h>

#include <stdint.h>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "atframe/connectors/atapp_connector_impl.h"

ATBUS_MACRO_NAMESPACE_BEGIN
class node;
class endpoint;
ATBUS_MACRO_NAMESPACE_END

LIBATAPP_MACRO_NAMESPACE_BEGIN

class atapp_connector_atbus : public atapp_connector_impl {
  UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connector_atbus)
  UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connector_atbus)

 private:
  struct atbus_connection_handle_data {
    atapp_connection_handle::ptr_t app_handle;
    atbus::topology_relation_type topology_relation;
    uint32_t flags;
    uint32_t reconnect_times;
    atfw::util::time::time_utility::raw_time_t reconnect_next_timepoint;
    atfw::util::time::time_utility::raw_time_t lost_topology_timeout;

    atbus::bus_id_t current_bus_id;
    atbus::bus_id_t upstream_bus_id;
    std::unordered_set<atbus::bus_id_t> downstream_bus_id;

    jiffies_timer_watcher_t timer_handle;
  };
  using atbus_connection_handle_ptr_t = atfw::util::memory::strong_rc_ptr<atbus_connection_handle_data>;
  using handle_map_t = std::unordered_map<uint64_t, atbus_connection_handle_ptr_t>;

 public:
  LIBATAPP_MACRO_API atapp_connector_atbus(app &owner);
  LIBATAPP_MACRO_API virtual ~atapp_connector_atbus();
  LIBATAPP_MACRO_API const char *name() noexcept override;
  LIBATAPP_MACRO_API void reload() noexcept;
  LIBATAPP_MACRO_API uint32_t get_address_type(const atbus::channel::channel_address_t &addr) const noexcept override;
  LIBATAPP_MACRO_API bool check_address_connectable(const atbus::channel::channel_address_t &addr,
                                                    const etcd_discovery_node &discovery) const noexcept override;
  LIBATAPP_MACRO_API int32_t on_start_listen(const atbus::channel::channel_address_t &addr) override;
  LIBATAPP_MACRO_API int32_t on_start_connect(const etcd_discovery_node &discovery, atapp_endpoint &endpoint,
                                              const atbus::channel::channel_address_t &addr,
                                              const atapp_connection_handle::ptr_t &handle) override;
  LIBATAPP_MACRO_API int32_t on_close_connection(atapp_connection_handle &handle) override;
  LIBATAPP_MACRO_API int32_t on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                     uint64_t *msg_sequence, gsl::span<const unsigned char> data,
                                                     const atapp::protocol::atapp_metadata *metadata) override;

  LIBATAPP_MACRO_API void on_discovery_event(etcd_discovery_action_t::type,
                                             const etcd_discovery_node::ptr_t &) override;

  LIBATAPP_MACRO_API void remove_topology_peer(atbus::bus_id_t target_bus_id);

  LIBATAPP_MACRO_API void update_topology_peer(atbus::bus_id_t target_bus_id, atbus::bus_id_t upstream_bus_id,
                                               atbus::topology_data::ptr_t data);

  using atapp_connector_impl::on_receive_forward_response;
  LIBATAPP_MACRO_API void on_receive_forward_response(uint64_t app_id, int32_t type, uint64_t msg_sequence,
                                                      int32_t error_code, gsl::span<const unsigned char> data,
                                                      const atapp::protocol::atapp_metadata *metadata);

 private:
  friend class app;
  int on_update_endpoint(const atbus::node &n, const atbus::endpoint *ep, int res);
  int on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res);
  int on_remove_endpoint(const atbus::node &n, atbus::endpoint *ep, int res);

  void set_handle_lost_topology(const atbus_connection_handle_ptr_t &handle);
  void set_handle_waiting_discovery(const atbus_connection_handle_ptr_t &handle);
  void resume_handle_discovery(const etcd_discovery_node &discovery);

  int32_t try_connect_to(const etcd_discovery_node &discovery, atapp_endpoint &endpoint,
                         const atbus::channel::channel_address_t *addr, const atapp_connection_handle::ptr_t &handle);

  int32_t on_start_connect_to_connected_endpoint(const etcd_discovery_node &discovery, atapp_endpoint &endpoint,
                                                 const atbus::channel::channel_address_t *addr,
                                                 const atapp_connection_handle::ptr_t &handle);

  int32_t on_start_connect_to_same_or_other_upstream_peer(const etcd_discovery_node &discovery,
                                                          atapp_endpoint &endpoint,
                                                          const atbus::channel::channel_address_t *addr,
                                                          const atapp_connection_handle::ptr_t &handle,
                                                          const atbus::topology_registry::ptr_t &topology_registry);

  int32_t on_start_connect_to_downstream_peer(const etcd_discovery_node &discovery, atapp_endpoint &endpoint,
                                              const atbus::channel::channel_address_t *addr,
                                              const atapp_connection_handle::ptr_t &handle,
                                              const atbus::topology_registry::ptr_t &topology_registry,
                                              atbus::topology_peer::ptr_t next_hop_peer);

  void remove_connection_handle(handle_map_t::iterator iter);

  void bind_connection_handle_upstream(atbus_connection_handle_data &downstream,
                                       atbus_connection_handle_data &upstream);

  void unbind_connection_handle_upstream(atbus_connection_handle_data &downstream,
                                         atbus_connection_handle_data &upstream);

 private:
  handle_map_t handles_;
  atbus::topology_data::ptr_t atbus_topology_data_;
  atbus::topology_policy_rule atbus_topology_policy_rule_;
  bool atbus_topology_policy_allow_direct_connection_;
};

LIBATAPP_MACRO_NAMESPACE_END
