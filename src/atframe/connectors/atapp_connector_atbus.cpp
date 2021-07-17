#include <detail/libatbus_error.h>

#include <atframe/atapp.h>
#include <atframe/connectors/atapp_connector_atbus.h>

namespace atapp {

LIBATAPP_MACRO_API atapp_connector_atbus::atapp_connector_atbus(app &owner) : atapp_connector_impl(owner) {
  register_protocol("mem");
  register_protocol("shm");
  register_protocol("unix");
  register_protocol("ipv4");
  register_protocol("ipv6");
  register_protocol("dns");
}

LIBATAPP_MACRO_API atapp_connector_atbus::~atapp_connector_atbus() { cleanup(); }

LIBATAPP_MACRO_API const char *atapp_connector_atbus::name() noexcept { return "atapp::connector.atapp"; }

LIBATAPP_MACRO_API uint32_t
atapp_connector_atbus::get_address_type(const atbus::channel::channel_address_t &addr) const {
  uint32_t ret = 0;
  if (atbus::channel::is_duplex_address(addr.address.c_str())) {
    ret |= address_type_t::EN_ACAT_DUPLEX;
  } else {
    ret |= address_type_t::EN_ACAT_SIMPLEX;
  }

  if (atbus::channel::is_local_host_address(addr.address.c_str())) {
    ret |= address_type_t::EN_ACAT_LOCAL_HOST;
  }

  if (atbus::channel::is_local_process_address(addr.address.c_str())) {
    ret |= address_type_t::EN_ACAT_LOCAL_PROCESS;
  }

  return ret;
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_start_listen(const atbus::channel::channel_address_t &addr) {
  if (nullptr == get_owner()) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  std::shared_ptr<atbus::node> node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  return node->listen(addr.address.c_str());
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_start_connect(const etcd_discovery_node *discovery,
                                                                   const atbus::channel::channel_address_t &addr,
                                                                   const atapp_connection_handle::ptr_t &handle) {
  if (nullptr == get_owner()) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  std::shared_ptr<atbus::node> node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  if (nullptr == discovery) {
    return EN_ATBUS_ERR_PARAMS;
  }

  uint64_t atbus_id = discovery->get_discovery_info().id();
  if (0 == atbus_id) {
    return EN_ATBUS_ERR_ATNODE_INVALID_ID;
  }

  // need connect atbus only if the node is parent or immediate family
  if (node->is_child_node(atbus_id)) {
    if (handle) {
      handles_[atbus_id] = handle;
      handle->set_ready();
      handle->set_private_data_u64(atbus_id);
    }
    return EN_ATAPP_ERR_SUCCESS;
  }

  do {
    const atbus::endpoint *parent_atbus_endpoint = node->get_parent_endpoint();
    if (nullptr == parent_atbus_endpoint) {
      break;
    }

    if (parent_atbus_endpoint->is_child_node(atbus_id)) {
      break;
    }

    // forward by parent node, skip connect
    if (handle) {
      handles_[atbus_id] = handle;
      handle->set_ready();
      handle->set_private_data_u64(atbus_id);
    }
    return EN_ATAPP_ERR_SUCCESS;
  } while (false);

  if (atbus::channel::is_local_host_address(addr.address.c_str()) &&
      node->get_hostname() != discovery->get_discovery_info().hostname()) {
    return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
  }

  if (atbus::channel::is_local_process_address(addr.address.c_str()) &&
      node->get_pid() != discovery->get_discovery_info().pid()) {
    return EN_ATBUS_ERR_CHANNEL_NOT_SUPPORT;
  }

  int res = node->connect(addr.address.c_str());
  if (res < 0) {
    return res;
  }

  if (handle) {
    handles_[atbus_id] = handle;
    handle->set_ready();
    handle->set_private_data_u64(atbus_id);
  }
  return EN_ATAPP_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_close_connect(atapp_connection_handle &handle) {
  // remove handle
  LIBATFRAME_UTILS_AUTO_SELETC_MAP(uint64_t, atapp_connection_handle::ptr_t)::iterator iter =
      handles_.find(handle.get_private_data_u64());
  if (iter != handles_.end() && iter->second.get() == &handle) {
    handles_.erase(iter);
  }
  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                                          uint64_t *sequence, const void *data,
                                                                          size_t data_size,
                                                                          const atapp::protocol::atapp_metadata *) {
  if (nullptr == get_owner() || nullptr == handle) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  std::shared_ptr<atbus::node> node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  return node->send_data(handle->get_private_data_u64(), type, data, data_size, sequence);
}

LIBATAPP_MACRO_API void atapp_connector_atbus::on_receive_forward_response(
    uint64_t app_id, int32_t type, uint64_t msg_sequence, int32_t error_code, const void *data, size_t data_size,
    const atapp::protocol::atapp_metadata *metadata) {
  LIBATFRAME_UTILS_AUTO_SELETC_MAP(uint64_t, atapp_connection_handle::ptr_t)::iterator iter = handles_.find(app_id);

  if (iter != handles_.end()) {
    atapp_connector_impl::on_receive_forward_response(iter->second.get(), type, msg_sequence, error_code, data,
                                                      data_size, metadata);
    return;
  }

  if (nullptr == get_owner()) {
    return;
  }

  // notify app
  app::message_t msg;
  msg.data = data;
  msg.data_size = data_size;
  msg.metadata = metadata;
  msg.msg_sequence = msg_sequence;
  msg.type = type;

  app::message_sender_t sender;
  sender.id = app_id;
  sender.remote = get_owner()->get_endpoint(app_id);
  if (nullptr != sender.remote) {
    sender.name = &sender.remote->get_name();
  }

  get_owner()->trigger_event_on_forward_response(sender, msg, error_code);
}

LIBATAPP_MACRO_API void atapp_connector_atbus::on_discovery_event(etcd_discovery_action_t::type,
                                                                  const etcd_discovery_node::ptr_t &) {}
}  // namespace atapp
