// Copyright 2026 atframework
//
// Created by owent

#pragma once

#include <nostd/nullability.h>
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

class atapp_connector_atbus : public atapp_connector_impl, public std::enable_shared_from_this<atapp_connector_atbus> {
  UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connector_atbus)
  UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connector_atbus)

 private:
  struct atbus_connection_handle_data;
  using atbus_connection_handle_ptr_t = atfw::util::memory::strong_rc_ptr<atbus_connection_handle_data>;
  using handle_map_t = std::unordered_map<uint64_t, atbus_connection_handle_ptr_t>;

 public:
  LIBATAPP_MACRO_API atapp_connector_atbus(app &owner);
  LIBATAPP_MACRO_API virtual ~atapp_connector_atbus();
  LIBATAPP_MACRO_API const char *name() noexcept override;
  LIBATAPP_MACRO_API void reload() noexcept;
  LIBATAPP_MACRO_API uint32_t get_address_type(const atbus::channel::channel_address_t &addr) const noexcept override;
  LIBATAPP_MACRO_API int32_t on_start_listen(const atbus::channel::channel_address_t &addr) override;
  LIBATAPP_MACRO_API int32_t on_start_connect(const etcd_discovery_node &discovery, atapp_endpoint &endpoint,
                                              const atbus::channel::channel_address_t &addr,
                                              const atapp_connection_handle::ptr_t &handle) override;
  LIBATAPP_MACRO_API int32_t on_close_connection(atapp_connection_handle &handle) override;
  LIBATAPP_MACRO_API int32_t on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                     uint64_t *msg_sequence, gsl::span<const unsigned char> data,
                                                     const atapp::protocol::atapp_metadata *metadata) override;

  LIBATAPP_MACRO_API void on_discovery_event(etcd_discovery_action_t, const etcd_discovery_node::ptr_t &) override;

  LIBATAPP_MACRO_API bool check_address_connectable(const atbus::channel::channel_address_t &addr,
                                                    const etcd_discovery_node &discovery) const noexcept;

  LIBATAPP_MACRO_API void remove_topology_peer(atbus::bus_id_t target_bus_id);

  LIBATAPP_MACRO_API void update_topology_peer(atbus::bus_id_t target_bus_id, atbus::bus_id_t upstream_bus_id,
                                               atbus::topology_data::ptr_t data);

  using atapp_connector_impl::on_receive_forward_response;
  LIBATAPP_MACRO_API void on_receive_forward_response(uint64_t app_id, int32_t type, uint64_t msg_sequence,
                                                      int32_t error_code, gsl::span<const unsigned char> data,
                                                      const atapp::protocol::atapp_metadata *metadata);

 private:
  friend class app;
  atfw::util::nostd::nonnull<atbus_connection_handle_ptr_t> create_connection_handle(
      atbus::bus_id_t bus_id, atbus::bus_id_t topology_upstream_bus_id, const atapp_connection_handle::ptr_t &handle);
  atfw::util::nostd::nonnull<atbus_connection_handle_ptr_t> mutable_connection_handle(
      atbus::bus_id_t bus_id, const atapp_connection_handle::ptr_t &handle);

  bool need_keep_handle(const atbus_connection_handle_data &handle_data) const noexcept;

  int on_update_endpoint(const atbus::node &n, const atbus::endpoint *ep, int res);
  int on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res);
  int on_remove_endpoint(const atbus::node &n, atbus::endpoint *ep, int res);
  void on_invalid_connection(const atbus::node &n, const atbus::connection *conn, int res);
  void on_new_connection(const atbus::node &n, const atbus::connection *conn);
  void on_close_connection(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn);
  void on_register(const atbus::node &n, const atbus::endpoint *ep, const atbus::connection *conn, int status);

  void set_handle_lost_topology(const atbus_connection_handle_ptr_t &handle);
  void set_handle_waiting_discovery(const atbus_connection_handle_ptr_t &handle);
  void resume_handle_discovery(const etcd_discovery_node &discovery);
  void set_handle_ready(const atbus_connection_handle_ptr_t &handle);
  void set_handle_unready(const atbus_connection_handle_ptr_t &handle);

  bool need_timer(const atbus_connection_handle_ptr_t &handle) const noexcept;
  void update_timer(const atbus_connection_handle_ptr_t &handle, atfw::util::time::time_utility::raw_time_t timeout);
  void remove_timer(const atbus_connection_handle_ptr_t &handle);

  bool setup_reconnect_timer(handle_map_t::iterator iter, std::chrono::system_clock::time_point previous_timeout);

  int32_t try_direct_reconnect(const atbus_connection_handle_ptr_t &handle);

  int32_t try_connect_to(const etcd_discovery_node &discovery, const atbus::channel::channel_address_t *addr,
                         const atapp_connection_handle::ptr_t &handle, bool allow_proxy);

  int32_t on_start_connect_to_connected_endpoint(const etcd_discovery_node &discovery,
                                                 const atbus::channel::channel_address_t *addr,
                                                 const atapp_connection_handle::ptr_t &handle);

  int32_t on_start_connect_to_same_or_other_upstream_peer(const etcd_discovery_node &discovery,
                                                          atbus::topology_relation_type relation,
                                                          const atbus::channel::channel_address_t *addr,
                                                          const atapp_connection_handle::ptr_t &handle,
                                                          const atbus::topology_registry::ptr_t &topology_registry,
                                                          bool allow_proxy);

  int32_t on_start_connect_to_upstream_peer(const etcd_discovery_node &discovery,
                                            const atbus::channel::channel_address_t *addr,
                                            const atapp_connection_handle::ptr_t &handle,
                                            const atbus::topology_registry::ptr_t &topology_registry,
                                            atbus::topology_peer::ptr_t next_hop_peer);

  int32_t on_start_connect_to_downstream_peer(const etcd_discovery_node &discovery,
                                              const atbus::channel::channel_address_t *addr,
                                              const atapp_connection_handle::ptr_t &handle,
                                              const atbus::topology_registry::ptr_t &topology_registry,
                                              atbus::topology_peer::ptr_t next_hop_peer);

  int32_t on_start_connect_to_proxy_by_upstream(const etcd_discovery_node &discovery,
                                                const atbus::channel::channel_address_t *addr,
                                                const atapp_connection_handle::ptr_t &handle,
                                                const atbus::topology_registry::ptr_t &topology_registry);

  void remove_connection_handle(handle_map_t::iterator iter);

  void bind_connection_handle_proxy(atbus_connection_handle_data &target, atbus_connection_handle_data &proxy);

  void unbind_connection_handle_proxy(atbus_connection_handle_data &target, atbus_connection_handle_data &proxy);

 private:
  handle_map_t handles_;
  atbus::topology_data::ptr_t atbus_topology_data_;
  atbus::topology_policy_rule atbus_topology_policy_rule_;
  atbus::bus_id_t last_connect_bus_id_;
  const atapp_connection_handle *last_connect_handle_;
  int32_t last_connect_result_;
};

LIBATAPP_MACRO_NAMESPACE_END
