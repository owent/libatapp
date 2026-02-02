// Copyright 2021 atframework
// Created by owent

#include <detail/libatbus_error.h>

#include <atframe/atapp.h>
#include <atframe/connectors/atapp_connector_atbus.h>

LIBATAPP_MACRO_NAMESPACE_BEGIN

namespace {
enum class atbus_connection_handle_flags_t : uint32_t {
  kNone = 0x00,
  // 主动连接(Active/Passive),本节点对其发消息为主动连接。如果仅仅是代理则是被动连接。
  kActiveConnection = 0x01,
  // 等待获取服务发现信息后再发起连接
  kWaitForDiscoveryToConnect = 0x02,
  // 拓扑信息已经移除，等待超时后需要unbind
  kLostTopology = 0x04,
  // 是否已就绪，如果是代理节点可能没有 atapp_connection_handle 可用, 无法通过下游判定是否就绪
  kReady = 0x08,
};

inline static bool check_flag(uint32_t flags, atbus_connection_handle_flags_t flag) {
  return (flags & static_cast<uint32_t>(flag)) != 0;
}

inline static void set_flag(uint32_t &flags, atbus_connection_handle_flags_t flag, bool v) {
  if (v) {
    flags |= static_cast<uint32_t>(flag);
  } else {
    flags &= ~static_cast<uint32_t>(flag);
  }
}

static bool check_atbus_endpoint_available(const atbus::node::ptr_t &node, atbus::bus_id_t id) noexcept {
  if (!node) {
    return false;
  }

  if (id == 0) {
    return false;
  }

  atbus::endpoint *ep = node->get_endpoint(id);
  if (ep == nullptr) {
    return false;
  }

  return ep->is_available();
}

}  // namespace

struct ATFW_UTIL_SYMBOL_LOCAL atapp_connector_atbus::atbus_connection_handle_data {
  atapp_connection_handle::ptr_t app_handle;
  uint32_t flags;
  uint32_t reconnect_times;
  atfw::util::time::time_utility::raw_time_t reconnect_next_timepoint;
  atfw::util::time::time_utility::raw_time_t lost_topology_timeout;

  atbus::bus_id_t current_bus_id;
  atbus::bus_id_t upstream_bus_id;
  std::unordered_set<atbus::bus_id_t> downstream_bus_id;

  jiffies_timer_watcher_t timer_handle;
  atfw::util::time::time_utility::raw_time_t pending_timer_timeout;
};

LIBATAPP_MACRO_API atapp_connector_atbus::atapp_connector_atbus(app &owner)
    : atapp_connector_impl(owner), atbus_topology_policy_allow_direct_connection_(false) {
  register_protocol("mem");
  register_protocol("shm");
  register_protocol("unix");
  register_protocol("pipe");
  register_protocol("ipv4");
  register_protocol("ipv6");
  register_protocol("dns");

  atbus_topology_data_ = atfw::util::memory::make_strong_rc<atbus::topology_data>();
}

LIBATAPP_MACRO_API atapp_connector_atbus::~atapp_connector_atbus() { cleanup(); }

LIBATAPP_MACRO_API const char *atapp_connector_atbus::name() noexcept { return "atapp::connector.atapp"; }

LIBATAPP_MACRO_API void atapp_connector_atbus::reload() noexcept {
  // checking atbus_topology_data changes
  const protocol::atbus_topology_data &topology_data_conf = get_owner()->get_origin_configure().bus().topology().data();
  bool changed = static_cast<int>(atbus_topology_data_->labels.size()) != topology_data_conf.label_size() ||
                 atbus_topology_data_->hostname != atbus::node::get_hostname() ||
                 atbus_topology_data_->pid != atbus::node::get_pid();
  for (auto &label : topology_data_conf.label()) {
    if (changed) {
      break;
    }

    auto iter = atbus_topology_data_->labels.find(label.first);
    if (iter == atbus_topology_data_->labels.end()) {
      changed = true;
      break;
    }
    if (iter->second != label.second) {
      changed = true;
      break;
    }
  }

  if (changed) {
    atbus_topology_data_->pid = atbus::node::get_pid();
    atbus_topology_data_->hostname = atbus::node::get_hostname();
    atbus_topology_data_->labels.clear();
    atbus_topology_data_->labels.reserve(static_cast<size_t>(topology_data_conf.label_size()));
    for (auto &label : topology_data_conf.label()) {
      atbus_topology_data_->labels[label.first] = label.second;
    }

    // TODO(owent): etcd update topology data to notify other nodes
  }

  // checking atbus_topology_data changes
  const protocol::atbus_topology_rule &topology_rule_conf = get_owner()->get_origin_configure().bus().topology().rule();
  atbus_topology_policy_rule_.require_same_process = topology_rule_conf.require_same_process();
  atbus_topology_policy_rule_.require_same_hostname = topology_rule_conf.require_same_host();
  atbus_topology_policy_rule_.require_label_values.clear();
  atbus_topology_policy_rule_.require_label_values.reserve(static_cast<size_t>(topology_rule_conf.match_label_size()));
  for (auto &label : topology_rule_conf.match_label()) {
    auto &values = atbus_topology_policy_rule_.require_label_values[label.first];
    values.clear();
    values.reserve(static_cast<size_t>(label.second.value_size()));
    for (auto &v : label.second.value()) {
      values.insert(v);
    }
  }
  atbus_topology_policy_allow_direct_connection_ = topology_rule_conf.allow_direct_connection();
}

LIBATAPP_MACRO_API uint32_t
atapp_connector_atbus::get_address_type(const atbus::channel::channel_address_t &addr) const noexcept {
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

LIBATAPP_MACRO_API bool atapp_connector_atbus::check_address_connectable(
    const atbus::channel::channel_address_t &addr, const etcd_discovery_node &discovery) const noexcept {
  // 单工地址指能作为数据通道，由atbus内部发起连接，不能作为初始控制通道
  if (atbus::channel::is_simplex_address(addr.address)) {
    return false;
  }

  if (atbus::channel::is_local_process_address(addr.address)) {
    if (discovery.get_discovery_info().pid() != atbus::node::get_pid()) {
      return false;
    }
  }

  if (atbus::channel::is_local_host_address(addr.address)) {
    if (discovery.get_discovery_info().hostname() != atbus::node::get_hostname()) {
      return false;
    }
  }

  return true;
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_start_listen(const atbus::channel::channel_address_t &addr) {
  atbus::node::ptr_t node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  return node->listen(addr.address.c_str());
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_start_connect(const etcd_discovery_node &discovery,
                                                                   atapp_endpoint &endpoint,
                                                                   const atbus::channel::channel_address_t &addr,
                                                                   const atapp_connection_handle::ptr_t &handle) {
  int32_t ret = try_connect_to(discovery, &addr, handle, true);
  if (handle) {
    // 上层过来的handle，一定是主动连接
    auto iter = handles_.find(handle->get_private_data_u64());
    if (iter != handles_.end() && iter->second) {
      set_flag(iter->second->flags, atbus_connection_handle_flags_t::kActiveConnection, true);
    }
  }
  return ret;
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_close_connection(atapp_connection_handle &handle) {
  uint64_t target_server_id = handle.get_private_data_u64();
  // remove handle
  handle_map_t::iterator iter = handles_.find(target_server_id);
  if (iter != handles_.end() && iter->second && iter->second->app_handle.get() == &handle) {
    set_handle_unready(iter->second);

    remove_connection_handle(iter);
  }

  return EN_ATBUS_ERR_SUCCESS;
}

LIBATAPP_MACRO_API int32_t atapp_connector_atbus::on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                                          uint64_t *sequence,
                                                                          gsl::span<const unsigned char> data,
                                                                          const atapp::protocol::atapp_metadata *) {
  if (nullptr == handle) {
    return EN_ATAPP_ERR_NOT_INITED;
  }

  atbus::node::ptr_t node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  atbus::node::send_data_options_t opts;
  if (sequence != nullptr) {
    opts.sequence = *sequence;
  }
  int32_t ret = node->send_data(handle->get_private_data_u64(), type, data, opts);
  if (sequence != nullptr) {
    *sequence = opts.sequence;
  }
  return ret;
}

LIBATAPP_MACRO_API void atapp_connector_atbus::on_receive_forward_response(
    uint64_t app_id, int32_t type, uint64_t msg_sequence, int32_t error_code, gsl::span<const unsigned char> data,
    const atapp::protocol::atapp_metadata *metadata) {
  handle_map_t::iterator iter = handles_.find(app_id);

  if (iter != handles_.end()) {
    atapp_connection_handle *app_handle = nullptr;
    if (iter->second) {
      app_handle = iter->second->app_handle.get();
    }
    atapp_connector_impl::on_receive_forward_response(app_handle, type, msg_sequence, error_code, data, metadata);
    return;
  }

  // notify app
  app::message_t msg;
  msg.data = data;
  msg.metadata = metadata;
  msg.message_sequence = msg_sequence;
  msg.type = type;

  app::message_sender_t sender;
  sender.id = app_id;
  sender.remote = get_owner()->get_endpoint(app_id);
  if (nullptr != sender.remote) {
    sender.name = sender.remote->get_name();
  }

  get_owner()->trigger_event_on_forward_response(sender, msg, error_code);
}

LIBATAPP_MACRO_API void atapp_connector_atbus::on_discovery_event(etcd_discovery_action_t::type action,
                                                                  const etcd_discovery_node::ptr_t &discovery) {
  // 服务发现信息上线，且该节点正在等待连接，则发起连接
  if (action == etcd_discovery_action_t::EN_NAT_PUT && discovery) {
    resume_handle_discovery(*discovery);
  } else if (discovery) {
    auto iter = handles_.find(discovery->get_discovery_info().id());
    if (iter != handles_.end() && iter->second) {
      // 服务发现信息删除，则标记为waiting_discovery。可能需要设置定时器等待重连
      set_handle_waiting_discovery(iter->second);
    }
  }
}

LIBATAPP_MACRO_API void atapp_connector_atbus::remove_topology_peer(atbus::bus_id_t target_bus_id) {
  handle_map_t::iterator iter = handles_.find(target_bus_id);
  if (iter == handles_.end()) {
    return;
  }

  if (!iter->second) {
    remove_connection_handle(iter);
    return;
  }

  set_handle_lost_topology(iter->second);
}

LIBATAPP_MACRO_API void atapp_connector_atbus::update_topology_peer(atbus::bus_id_t target_bus_id,
                                                                    atbus::bus_id_t upstream_bus_id,
                                                                    atbus::topology_data::ptr_t data) {
  // 拓扑关系刷新
  handle_map_t::iterator iter = handles_.find(target_bus_id);
  if (iter == handles_.end()) {
    return;
  }

  auto handle = iter->second;
  if (!handle) {
    remove_connection_handle(iter);
    return;
  }
  set_flag(handle->flags, atbus_connection_handle_flags_t::kLostTopology, false);

  // 如果不再需要定时器（非丢失信息也非正在重连），则移除定时器
  if (!need_timer(handle)) {
    remove_timer(handle);
  }

  // 如果拓扑信息未变化，那么数据链路也没变化，直接返回即可
  if (handle->upstream_bus_id == upstream_bus_id) {
    // 如果在等待服务发现信息，且之前因为缺失拓扑数据而放弃了，那么要立即发起重连
    try_direct_reconnect(handle);
    return;
  }

  // 解绑旧的上游连接
  do {
    if (handle->upstream_bus_id == 0) {
      break;
    }
    handle_map_t::iterator upstream_iter = handles_.find(handle->upstream_bus_id);
    if (upstream_iter == handles_.end()) {
      // inconsistent state
      FWLOGERROR("inconsistent state when update_topology_peer for target bus id {:#x}, upstream bus id {:#x}",
                 target_bus_id, handle->upstream_bus_id);
      break;
    }
    if (!upstream_iter->second) {
      remove_connection_handle(upstream_iter);
      // inconsistent state
      FWLOGERROR("inconsistent state when update_topology_peer for target bus id {:#x}, upstream bus id {:#x}",
                 target_bus_id, handle->upstream_bus_id);
      break;
    }
    unbind_connection_handle_upstream(*handle, *upstream_iter->second);
  } while (false);

  // 有直连atbus::endpoint, 则直接使用,不需要发起重连
  if (check_atbus_endpoint_available(get_owner()->get_bus_node(), target_bus_id)) {
    set_handle_ready(handle);
    return;
  }

  if (!need_keep_handle(*handle)) {
    // 刷新迭代器
    iter = handles_.find(target_bus_id);
    if (iter == handles_.end()) {
      return;
    }
    remove_connection_handle(iter);
    return;
  }

  // 先走一次更新上游的快速绑定
  if (upstream_bus_id != 0) {
    handle_map_t::iterator upstream_iter = handles_.find(upstream_bus_id);
    if (upstream_iter != handles_.end() && upstream_iter->second) {
      bind_connection_handle_upstream(*handle, *upstream_iter->second);

      if (check_flag(upstream_iter->second->flags, atbus_connection_handle_flags_t::kReady)) {
        set_handle_ready(handle);
      } else {
        set_handle_unready(handle);
      }
      return;
    }
  }

  // 正在等待服务发现信息，跳过
  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect)) {
    set_handle_unready(handle);
    return;
  }
  auto discovery_info = get_owner()->get_discovery_node_by_id(target_bus_id);
  if (!discovery_info) {
    set_handle_waiting_discovery(handle);
    set_handle_unready(handle);
    return;
  }

  // 重算链路,仅主动连接的目标允许尝试代理连接
  try_connect_to(*discovery_info, nullptr, handle->app_handle,
                 check_flag(handle->flags, atbus_connection_handle_flags_t::kActiveConnection));

  // 如果未就绪，需要重设定时器
  if (!check_flag(handle->flags, atbus_connection_handle_flags_t::kReady)) {
    // 刷新迭代器
    iter = handles_.find(target_bus_id);
    if (iter == handles_.end()) {
      return;
    }
    setup_reconnect_timer(iter, std::chrono::system_clock::from_time_t(0));
  }
}

atapp_connector_atbus::atbus_connection_handle_ptr_t atapp_connector_atbus::create_connection_handle(
    atbus::bus_id_t bus_id, const atapp_connection_handle::ptr_t &handle) {
  auto data = atfw::util::memory::make_strong_rc<atapp_connector_atbus::atbus_connection_handle_data>();
  data->app_handle = handle;
  data->flags = static_cast<uint32_t>(atbus_connection_handle_flags_t::kNone);
  data->reconnect_times = 0;
  data->reconnect_next_timepoint = std::chrono::system_clock::from_time_t(0);
  data->lost_topology_timeout = std::chrono::system_clock::from_time_t(0);
  data->current_bus_id = bus_id;
  data->upstream_bus_id = 0;

  data->pending_timer_timeout = std::chrono::system_clock::from_time_t(0);
  return data;
}

bool atapp_connector_atbus::need_keep_handle(const atbus_connection_handle_data &handle_data) const noexcept {
  if (check_flag(handle_data.flags, atbus_connection_handle_flags_t::kActiveConnection) ||
      !handle_data.downstream_bus_id.empty()) {
    return true;
  }

  // 本身没有连接句柄，说明作为代理节点使用，不会有pending message
  if (handle_data.app_handle == nullptr) {
    return false;
  }

  atapp_endpoint *ep = handle_data.app_handle->get_endpoint();
  if (ep == nullptr) {
    return false;
  }

  return ep->get_pending_message_count() > 0;
}

int atapp_connector_atbus::on_update_endpoint(const atbus::node &n, const atbus::endpoint *ep, int res) {
  if (ep == nullptr) {
    return res;
  }

  auto iter = handles_.find(ep->get_id());
  // Maybe destroyed
  if (iter == handles_.end()) {
    return res;
  }

  if (!iter->second) {
    remove_connection_handle(iter);
    return res;
  }

  // 连接失败直接忽略即可，定时器互触发重试和超时删除
  if (res < 0) {
    return res;
  }

  // 如果没有可用的数据连接，则不处理继续等待
  if (!ep->is_available()) {
    return res;
  }

  auto handle = iter->second;
  set_handle_ready(handle);

  // 如果不再需要定时器（非丢失信息也非正在重连），则移除定时器
  if (!need_timer(handle)) {
    remove_timer(handle);
  }
  return res;
}

int atapp_connector_atbus::on_add_endpoint(const atbus::node &n, atbus::endpoint *ep, int res) {
  return on_update_endpoint(n, ep, res);
}

int atapp_connector_atbus::on_remove_endpoint(const atbus::node &, atbus::endpoint *ep, int res) {
  if (ep == nullptr) {
    return res;
  }

  auto iter = handles_.find(ep->get_id());
  // Maybe destroyed
  if (iter == handles_.end()) {
    return res;
  }

  auto handle = iter->second;
  if (!handle) {
    remove_connection_handle(iter);
    return res;
  }

  // 不需要重连则直接移除handle
  if (!need_keep_handle(*handle)) {
    remove_connection_handle(iter);
    return res;
  }

  // 要设置重连定时器，上游和下游节点也要依靠定时器做超时清理
  if (!setup_reconnect_timer(iter, std::chrono::system_clock::from_time_t(0))) {
    return res;
  }

  atbus::topology_relation_type topology_relation = atbus::topology_relation_type::kInvalid;
  if (get_owner()->get_bus_node()) {
    topology_relation = get_owner()->get_bus_node()->get_topology_relation(ep->get_id(), nullptr);
  }

  do {
    // 如果不是正在等待连接，且是邻居/远方节点，则再检查一次是否允许直连。
    if (topology_relation != atbus::topology_relation_type::kSameUpstreamPeer &&
        topology_relation != atbus::topology_relation_type::kOtherUpstreamPeer) {
      break;
    }

    if (check_flag(handle->flags, atbus_connection_handle_flags_t::kLostTopology)) {
      break;
    }

    if (check_flag(handle->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect)) {
      break;
    }

    if (EN_ATAPP_ERR_TOPOLOGY_DENY == try_direct_reconnect(handle)) {
      // 直连不允许，移除handle
      remove_connection_handle(iter);
      return res;
    }
  } while (false);

  return res;
}

void atapp_connector_atbus::set_handle_lost_topology(const atbus_connection_handle_ptr_t &handle) {
  if (!handle) {
    return;
  }
  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kLostTopology)) {
    return;
  }

  set_flag(handle->flags, atbus_connection_handle_flags_t::kLostTopology, true);

  auto &conf = get_owner()->get_origin_configure().bus();
  std::chrono::microseconds lost_topology_timeout;
  protobuf_to_chrono_set_duration(lost_topology_timeout, conf.lost_topology_timeout());
  if (lost_topology_timeout <= std::chrono::microseconds(0)) {
    lost_topology_timeout = std::chrono::seconds(120);
  }
  update_timer(handle, get_owner()->get_sys_now() +
                           std::chrono::duration_cast<std::chrono::system_clock::duration>(lost_topology_timeout));
}

void atapp_connector_atbus::set_handle_waiting_discovery(const atbus_connection_handle_ptr_t &handle) {
  if (!handle) {
    return;
  }

  atbus::topology_relation_type topology_relation = atbus::topology_relation_type::kInvalid;
  if (get_owner()->get_bus_node()) {
    topology_relation = get_owner()->get_bus_node()->get_topology_relation(handle->current_bus_id, nullptr);
  }
  switch (topology_relation) {
    // 上游节点由atbus层自动重连，不需要等待服务发现数据
    case atbus::topology_relation_type::kImmediateUpstream:
    case atbus::topology_relation_type::kTransitiveUpstream:
    // 下游节点被动连接，不需要等待服务发现数据
    case atbus::topology_relation_type::kImmediateDownstream:
    case atbus::topology_relation_type::kTransitiveDownstream:
      return;

    default:
      break;
  }

  // 如果通过邻居/远方节点上游转发，也不需要等待当前服务发现重连
  if (handle->upstream_bus_id != 0) {
    return;
  }

  // 如果当前已有连接且连接可用，则不需要等待服务发现重连
  if (check_atbus_endpoint_available(get_owner()->get_bus_node(), handle->current_bus_id)) {
    return;
  }

  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect)) {
    return;
  }

  set_flag(handle->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect, true);
}

void atapp_connector_atbus::resume_handle_discovery(const etcd_discovery_node &discovery) {
  auto iter = handles_.find(discovery.get_discovery_info().id());
  if (iter == handles_.end()) {
    return;
  }
  if (!iter->second) {
    remove_connection_handle(iter);
    return;
  }
  if (!check_flag(iter->second->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect)) {
    return;
  }
  // 移除等待服务发现数据标记
  set_flag(iter->second->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect, false);

  // 发起重连
  try_direct_reconnect(iter->second);
}

void atapp_connector_atbus::set_handle_ready(const atbus_connection_handle_ptr_t &handle) {
  if (!handle) {
    return;
  }

  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kReady)) {
    return;
  }
  set_flag(handle->flags, atbus_connection_handle_flags_t::kReady, true);
  handle->reconnect_times = 0;

  if (handle->app_handle != nullptr) {
    handle->app_handle->set_ready();
    auto endpoint = handle->app_handle->get_endpoint();
    if (nullptr != endpoint) {
      if (endpoint->get_pending_message_size() > 0) {
        FWLOGINFO("bus node {:#x} add_waker for {:#x} with {} pending messages(size: {})", get_owner()->get_app_id(),
                  endpoint->get_id(), endpoint->get_pending_message_count(), endpoint->get_pending_message_size());
        endpoint->add_waker(get_owner()->get_next_tick_time());
      }
    }
  }

  // 代理节点可用时要触发被代理节点的handle.set_ready() + endpoint::add_waker
  std::unordered_set<uint64_t> invalid_downstream;
  // Copy一份ID，防止递归调用的时候修改集合
  std::unordered_set<atbus::bus_id_t> copy_downstream_bus_id = handle->downstream_bus_id;
  for (auto downstream_handle_id : copy_downstream_bus_id) {
    auto iter = handles_.find(downstream_handle_id);
    if (iter == handles_.end()) {
      invalid_downstream.insert(downstream_handle_id);
      continue;
    }
    set_handle_ready(iter->second);
  }

  for (auto id : invalid_downstream) {
    handle->downstream_bus_id.erase(id);
  }
}

void atapp_connector_atbus::set_handle_unready(const atbus_connection_handle_ptr_t &handle) {
  if (!handle) {
    return;
  }

  if (!check_flag(handle->flags, atbus_connection_handle_flags_t::kReady)) {
    return;
  }
  set_flag(handle->flags, atbus_connection_handle_flags_t::kReady, false);

  if (handle->app_handle) {
    handle->app_handle->set_unready();
  }

  // 代理节点重连期间，被代理下游节点的 handle.set_unready()
  std::unordered_set<uint64_t> invalid_downstream;
  std::unordered_set<atbus::bus_id_t> copy_downstream_bus_id = handle->downstream_bus_id;
  // Copy一份ID，防止递归调用的时候修改集合
  for (auto downstream_handle_id : copy_downstream_bus_id) {
    auto iter = handles_.find(downstream_handle_id);
    if (iter == handles_.end()) {
      invalid_downstream.insert(downstream_handle_id);
      continue;
    }
    set_handle_unready(iter->second);
  }

  for (auto id : invalid_downstream) {
    handle->downstream_bus_id.erase(id);
  }
}

bool atapp_connector_atbus::need_timer(const atbus_connection_handle_ptr_t &handle) const noexcept {
  if (!handle) {
    return false;
  }

  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kLostTopology)) {
    return true;
  }

  // 如果处于就绪状态则不需要重连和移除
  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kReady)) {
    return false;
  }

  // atbus直连可用，不需要重连和移除（作为代理，可能没atapp_endpoint）
  if (check_atbus_endpoint_available(get_owner()->get_bus_node(), handle->current_bus_id)) {
    return false;
  }

  return true;
}

void atapp_connector_atbus::update_timer(const atbus_connection_handle_ptr_t &handle,
                                         atfw::util::time::time_utility::raw_time_t timeout) {
  if (!handle) {
    return;
  }

  if (handle->pending_timer_timeout != std::chrono::system_clock::from_time_t(0) && !handle->timer_handle.expired() &&
      handle->pending_timer_timeout <= timeout) {
    return;
  }

  remove_timer(handle);
  atfw::util::memory::weak_rc_ptr<atbus_connection_handle_data> weak_handle(handle);
  atfw::util::memory::weak_rc_ptr<atapp_connector_atbus> weak_self(shared_from_this());

  get_owner()->add_custom_timer(
      timeout,
      [weak_handle, weak_self](time_t /*tick_time*/, const jiffies_timer_t::timer_t &) {
        atbus_connection_handle_ptr_t h = weak_handle.lock();
        if (!h) {
          return;
        }

        h->pending_timer_timeout = std::chrono::system_clock::from_time_t(0);
        h->timer_handle.reset();

        atfw::util::memory::strong_rc_ptr<atapp_connector_atbus> self = weak_self.lock();
        if (!self) {
          return;
        }

        // 检查handle是否仍然有效
        auto iter = self->handles_.find(h->current_bus_id);
        if (iter == self->handles_.end()) {
          return;
        }
        if (iter->second != h) {
          return;
        }

        std::chrono::system_clock::time_point sys_now = self->get_owner()->get_sys_now();
        std::chrono::system_clock::time_point next_timer_timeout = std::chrono::system_clock::from_time_t(0);

        // 处理丢失拓扑后的重连超时则直接移除
        if (check_flag(h->flags, atbus_connection_handle_flags_t::kLostTopology)) {
          if (sys_now >= h->lost_topology_timeout) {
            self->remove_connection_handle(iter);
            return;
          } else {
            next_timer_timeout = h->lost_topology_timeout;
          }
        }

        // 如果处于就绪状态则不需要重连和移除
        if (check_flag(h->flags, atbus_connection_handle_flags_t::kReady)) {
          if (next_timer_timeout != std::chrono::system_clock::from_time_t(0)) {
            self->update_timer(h, next_timer_timeout);
          }
          return;
        }

        if (!self->setup_reconnect_timer(iter, next_timer_timeout)) {
          return;
        }
        self->try_direct_reconnect(h);
      },
      &handle->timer_handle);
}

void atapp_connector_atbus::remove_timer(const atbus_connection_handle_ptr_t &handle) {
  if (!handle) {
    return;
  }

  if (handle->pending_timer_timeout == std::chrono::system_clock::from_time_t(0)) {
    return;
  }

  handle->pending_timer_timeout = std::chrono::system_clock::from_time_t(0);
  get_owner()->remove_custom_timer(handle->timer_handle);
}

bool atapp_connector_atbus::setup_reconnect_timer(handle_map_t::iterator iter,
                                                  std::chrono::system_clock::time_point previous_timeout) {
  if (iter == handles_.end()) {
    return false;
  }

  if (!iter->second) {
    remove_connection_handle(iter);
    return false;
  }

  // atbus直连可用，不需要重连和移除（作为代理，可能没atapp_endpoint）
  if (check_atbus_endpoint_available(get_owner()->get_bus_node(), iter->second->current_bus_id)) {
    if (previous_timeout != std::chrono::system_clock::from_time_t(0)) {
      update_timer(iter->second, previous_timeout);
    }
    return true;
  }

  // 重连次数超出限制，直接移除
  auto &conf = get_owner()->get_origin_configure().bus();
  if (conf.reconnect_max_try_times() > 0 && iter->second->reconnect_times >= conf.reconnect_max_try_times()) {
    remove_connection_handle(iter);
    return false;
  }

  std::chrono::microseconds reconnect_max_interval;
  std::chrono::microseconds reconnect_cur_interval;
  protobuf_to_chrono_set_duration(reconnect_max_interval, conf.reconnect_max_interval());
  if (reconnect_max_interval <= std::chrono::microseconds(0)) {
    reconnect_max_interval = std::chrono::seconds(60);
  }
  if (reconnect_cur_interval <= std::chrono::microseconds(0)) {
    reconnect_cur_interval = std::chrono::seconds(8);
  }
  protobuf_to_chrono_set_duration(reconnect_cur_interval, conf.reconnect_start_interval());
  uint32_t calc_interval = iter->second->reconnect_times++;
  while (calc_interval > 0) {
    reconnect_cur_interval *= 2;
    if (reconnect_cur_interval >= reconnect_max_interval) {
      reconnect_cur_interval = reconnect_max_interval;
      break;
    }
    --calc_interval;
  }
  auto sys_now = get_owner()->get_sys_now();
  if (previous_timeout == std::chrono::system_clock::from_time_t(0) ||
      sys_now + reconnect_cur_interval < previous_timeout) {
    previous_timeout =
        sys_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(reconnect_cur_interval);
  }

  update_timer(iter->second, previous_timeout);
  return true;
}

int32_t atapp_connector_atbus::try_direct_reconnect(const atbus_connection_handle_ptr_t &handle) {
  if (!handle) {
    return EN_ATAPP_ERR_SUCCESS;
  }

  // 如果已经是ready状态，则不需要重连
  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kReady)) {
    return EN_ATAPP_ERR_SUCCESS;
  }

  // 丢失拓扑信息，等待数据恢复后再尝试连接，否则当前的链路可能是错的
  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kLostTopology)) {
    return EN_ATAPP_ERR_SUCCESS;
  }

  atbus::topology_relation_type topology_relation = atbus::topology_relation_type::kInvalid;
  if (get_owner()->get_bus_node()) {
    topology_relation = get_owner()->get_bus_node()->get_topology_relation(handle->current_bus_id, nullptr);
  }
  switch (topology_relation) {
    // 上游节点由atbus层自动重连，不需要通过服务发现数据主动连接
    case atbus::topology_relation_type::kImmediateUpstream:
    case atbus::topology_relation_type::kTransitiveUpstream:
    // 下游节点被动连接
    case atbus::topology_relation_type::kImmediateDownstream:
    case atbus::topology_relation_type::kTransitiveDownstream:
      return EN_ATAPP_ERR_SUCCESS;

    default:
      break;
  }

  // 如果通过邻居/远方节点上游转发，也不需要等待当前服务发现重连
  if (handle->upstream_bus_id != 0) {
    return;
  }

  // 如果正在等待服务发现数据，则先不用发起连接，等待数据
  if (check_flag(handle->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect)) {
    return EN_ATAPP_ERR_SUCCESS;
  }

  auto discovery = get_owner()->get_discovery_node_by_id(handle->current_bus_id);
  if (!discovery) {
    set_flag(handle->flags, atbus_connection_handle_flags_t::kWaitForDiscoveryToConnect, false);
    return EN_ATAPP_ERR_DISCOVERY_NOT_FOUND;
  }

  int32_t ret = try_connect_to(*discovery, nullptr, handle->app_handle, false);
  if (ret < 0) {
    FWLOGERROR("reconnect to bus id {:#x} failed with error code {}", handle->current_bus_id, ret);
  }

  return ret;
}

int32_t atapp_connector_atbus::try_connect_to(const etcd_discovery_node &discovery,
                                              const atbus::channel::channel_address_t *addr,
                                              const atapp_connection_handle::ptr_t &handle, bool allow_proxy) {
  int32_t ret = on_start_connect_to_connected_endpoint(discovery, addr, handle);
  if (ret == EN_ATAPP_ERR_SUCCESS || ret != EN_ATAPP_ERR_TRY_NEXT) {
    return ret;
  }

  atbus::node::ptr_t node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  uint64_t atbus_id = discovery.get_discovery_info().id();
  if (0 == atbus_id) {
    return EN_ATBUS_ERR_ATNODE_INVALID_ID;
  }
  atbus::topology_peer::ptr_t next_hop_try_peer = nullptr;
  atbus::topology_relation_type relation = node->get_topology_relation(atbus_id, &next_hop_try_peer);
  // 如果没有拓扑关系，不允许连接
  if (relation == atbus::topology_relation_type::kInvalid) {
    return EN_ATAPP_ERR_TOPOLOGY_UNKNOWN;
  }

  // 只有[邻居/远方节点]允许主动直连
  const atbus::topology_registry::ptr_t &topology_registry = node->get_topology_registry();
  if ((relation == atbus::topology_relation_type::kOtherUpstreamPeer ||
       relation == atbus::topology_relation_type::kSameUpstreamPeer) &&
      atbus_topology_policy_allow_direct_connection_) {
    ret = on_start_connect_to_same_or_other_upstream_peer(discovery, addr, handle, topology_registry, allow_proxy);
    if (ret == EN_ATAPP_ERR_SUCCESS || ret != EN_ATAPP_ERR_TRY_NEXT) {
      return ret;
    }
  }

  // TODO: 按拓扑关系 - 直连上游（间接上游）
  // 按拓扑关系 - 直连下游/间接下游
  if (relation == atbus::topology_relation_type::kImmediateDownstream ||
      relation == atbus::topology_relation_type::kTransitiveDownstream) {
    ret = on_start_connect_to_downstream_peer(discovery, addr, handle, topology_registry, next_hop_try_peer);
    if (ret == EN_ATAPP_ERR_SUCCESS || ret != EN_ATAPP_ERR_TRY_NEXT) {
      return ret;
    }
  }

  if (!allow_proxy) {
    return EN_ATAPP_ERR_NO_AVAILABLE_ADDRESS;
  }

  // TODO: 按拓扑关系 - 直接上游转发

  // TODO: 是否需要等待新连接

  // need connect atbus only if the node is parent or immediate family
  if (relation == atbus::topology_relation_type::kImmediateDownstream ||
      relation == atbus::topology_relation_type::kTransitiveDownstream) {
    if (handle) {
      handles_[atbus_id] = handle;
      handle->set_private_data_u64(atbus_id);

      if (atbus_id == get_owner()->get_app_id() || node->is_endpoint_available(atbus_id)) {
        set_handle_ready(handle);
      }
    }
    return EN_ATAPP_ERR_SUCCESS;
  }
}

int32_t atapp_connector_atbus::on_start_connect_to_connected_endpoint(const etcd_discovery_node &discovery,
                                                                      const atbus::channel::channel_address_t *addr,
                                                                      const atapp_connection_handle::ptr_t &handle) {
  atbus::node::ptr_t node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  uint64_t atbus_id = discovery.get_discovery_info().id();
  if (0 == atbus_id) {
    return EN_ATBUS_ERR_ATNODE_INVALID_ID;
  }

  // 自己总是允许
  if (node->get_id() == atbus_id) {
    if (handle) {
      handles_[atbus_id] = handle;
      handle->set_private_data_u64(atbus_id);
      set_handle_ready(handle);
    }
    return EN_ATAPP_ERR_SUCCESS;
  }

  // 直接上游总是允许
  if (node->get_upstream_endpoint() && node->get_upstream_endpoint()->get_id() == atbus_id) {
    const atbus::endpoint *ep = node->get_upstream_endpoint();
    if (handle) {
      handles_[atbus_id] = handle;
      handle->set_private_data_u64(atbus_id);

      const atbus::endpoint *self_ep = node->get_self_endpoint();
      // 如果数据通道已经建立，则直接设为就绪。否则等待数据通道建立完成后再发送数据
      if (self_ep->get_ctrl_connection(ep) != nullptr && self_ep->get_data_connection(ep, false) != nullptr) {
        set_handle_ready(handle);
      } else {
        set_handle_unready(handle);
      }
    }
    return EN_ATAPP_ERR_SUCCESS;
  }

  // 优先使用已有直连
  const atbus::endpoint *ep = node->get_endpoint(atbus_id);
  if (ep != nullptr) {
    handles_[atbus_id] = handle;
    handle->set_private_data_u64(atbus_id);

    const atbus::endpoint *self_ep = node->get_self_endpoint();
    // 如果数据通道已经建立，则直接设为就绪。否则等待数据通道建立完成后再发送数据
    if (self_ep->get_ctrl_connection(ep) != nullptr && self_ep->get_data_connection(ep, false) != nullptr) {
      set_handle_ready(handle);
    } else {
      set_handle_unready(handle);
    }
    return EN_ATAPP_ERR_SUCCESS;
  }

  return EN_ATAPP_ERR_TRY_NEXT;
}

int32_t atapp_connector_atbus::on_start_connect_to_same_or_other_upstream_peer(
    const etcd_discovery_node &discovery, const atbus::channel::channel_address_t *addr,
    const atapp_connection_handle::ptr_t &handle, const atbus::topology_registry::ptr_t &topology_registry,
    bool allow_proxy) {
  atbus::node::ptr_t node = get_owner()->get_bus_node();
  if (!node) {
    return EN_ATAPP_ERR_SETUP_ATBUS;
  }

  uint64_t atbus_id = discovery.get_discovery_info().id();
  if (0 == atbus_id) {
    return EN_ATBUS_ERR_ATNODE_INVALID_ID;
  }

  atbus::topology_peer::ptr_t target_peer = topology_registry->get_peer(atbus_id);
  if (!target_peer) {
    return EN_ATAPP_ERR_TOPOLOGY_UNKNOWN;
  }
  // 找到可用的直连代理ID和地址
  atbus::topology_peer::ptr_t proxy_peer = target_peer;

  atbus::channel::channel_address_t proxy_addr;
  for (; proxy_peer; proxy_peer = proxy_peer->get_upstream()) {
    // 不符合策略要求，跳过
    if (!topology_registry->check_policy(atbus_topology_policy_rule_, *atbus_topology_data_,
                                         proxy_peer->get_topology_data())) {
      if (allow_proxy) {
        continue;
      } else {
        return EN_ATAPP_ERR_TOPOLOGY_DENY;
      }
    }

    // 当前节点直连传入的地址必然有效
    if (addr != nullptr && proxy_peer->get_bus_id() == atbus_id) {
      proxy_addr = *addr;
      break;
    }

    // 如果代理上游已经连接成功则直接使用
    const atbus::endpoint *ep = node->get_endpoint(atbus_id);
    if (ep != nullptr) {
      handles_[atbus_id] = handle;
      handle->set_private_data_u64(atbus_id);

      const atbus::endpoint *self_ep = node->get_self_endpoint();
      // 如果数据通道已经建立，则直接设为就绪。否则等待数据通道建立完成后再发送数据
      if (self_ep->get_ctrl_connection(ep) != nullptr && self_ep->get_data_connection(ep, false) != nullptr) {
        set_handle_ready(handle);
      } else {
        set_handle_unready(handle);
      }
      return EN_ATAPP_ERR_SUCCESS;
    }

    // 查询可用的地址
    etcd_discovery_node::ptr_t proxy_discovery = get_owner()->get_discovery_node_by_id(proxy_peer->get_bus_id());
    if (!proxy_discovery) {
      continue;
    }

    int32_t gateway_size = proxy_discovery->get_ingress_size();
    proxy_discovery->reset_ingress_index();
    for (int32_t i = 0; handle && i < gateway_size; ++i) {
      atbus::channel::channel_address_t parsed_addr;
      const atapp::protocol::atapp_gateway &gateway = proxy_discovery->next_ingress_gateway();
      if (!get_owner()->match_gateway(gateway)) {
        continue;
      }
      atbus::channel::make_address(gateway.address().c_str(), parsed_addr);
      if (!check_address_connectable(parsed_addr, *proxy_discovery)) {
        continue;
      }

      proxy_addr = parsed_addr;
      break;
    }

    if (!proxy_addr.address.empty()) {
      break;
    }
  }

  if (proxy_addr.address.empty() || !proxy_peer) {
    return EN_ATAPP_ERR_TRY_NEXT;
  }

  // TODO: 如果在重连等待期内则直接返回成功等待连接完成
  // TODO: 否则发起直连,设置重连定时器
  return EN_ATAPP_ERR_TRY_NEXT;
}

int32_t atapp_connector_atbus::on_start_connect_to_downstream_peer(
    const etcd_discovery_node &discovery, const atbus::channel::channel_address_t *addr,
    const atapp_connection_handle::ptr_t &handle, const atbus::topology_registry::ptr_t &topology_registry,
    atbus::topology_peer::ptr_t next_hop_peer) {
  if (!next_hop_peer) {
    return EN_ATAPP_ERR_TRY_NEXT;
  }

  // TODO: 下游节点，等待直接下游连接成功即可
  return EN_ATAPP_ERR_TRY_NEXT;
}

void atapp_connector_atbus::remove_connection_handle(handle_map_t::iterator iter) {
  if (iter == handles_.end()) {
    return;
  }

  handle_map_t::mapped_type handle_data = iter->second;
  handles_.erase(iter);

  // endpoint内的pending消息会在析构时执行取消
  if (!handle_data) {
    return;
  }

  // 移除代理关系的上游关系
  if (handle_data->upstream_bus_id != 0) {
    auto upstream_iter = handles_.find(handle_data->upstream_bus_id);
    if (upstream_iter != handles_.end() && upstream_iter->second) {
      unbind_connection_handle_upstream(*handle_data, *upstream_iter->second);
    }
  }

  // 移除代理关系的下游handle
  std::unordered_set<atbus::bus_id_t> copy_downstream_bus_id = handle_data->downstream_bus_id;
  for (auto downstream_bus_id : copy_downstream_bus_id) {
    auto downstream_iter = handles_.find(downstream_bus_id);
    if (downstream_iter != handles_.end() && downstream_iter->second) {
      atbus_connection_handle_ptr_t downstream_handle = downstream_iter->second;
      if (downstream_handle->upstream_bus_id != handle_data->current_bus_id) {
        continue;
      }
      unbind_connection_handle_upstream(*downstream_handle, *handle_data);

      on_close_connection(*downstream_handle->app_handle);
    }
  }
}

void atapp_connector_atbus::bind_connection_handle_upstream(atbus_connection_handle_data &downstream,
                                                            atbus_connection_handle_data &upstream) {
  if (downstream.upstream_bus_id == upstream.current_bus_id) {
    return;
  }

  if (downstream.upstream_bus_id != 0) {
    auto iter = handles_.find(downstream.upstream_bus_id);
    if (iter != handles_.end() && iter->second) {
      unbind_connection_handle_upstream(downstream, *iter->second);
    }
  }

  downstream.upstream_bus_id = upstream.current_bus_id;
  upstream.downstream_bus_id.insert(downstream.current_bus_id);

  FWLOGINFO("atbus bind upstream {:#x} --> {:#x}", downstream.current_bus_id, upstream.current_bus_id);
}

void atapp_connector_atbus::unbind_connection_handle_upstream(atbus_connection_handle_data &downstream,
                                                              atbus_connection_handle_data &upstream) {
  if (downstream.upstream_bus_id == upstream.current_bus_id) {
    downstream.upstream_bus_id = 0;
    FWLOGINFO("atbus unbind upstream {:#x} --x--> {:#x}", downstream.current_bus_id, upstream.current_bus_id);
  }

  upstream.downstream_bus_id.erase(downstream.current_bus_id);

  // 如果非直连且下游清空了，那么可以直接 disconnect
  if (upstream.downstream_bus_id.empty() &&
      !check_flag(upstream.flags, atbus_connection_handle_flags_t::kActiveConnection)) {
    atbus::node::ptr_t node = get_owner()->get_bus_node();
    if (node) {
      const atbus::endpoint *ep = node->get_endpoint(upstream.current_bus_id);
      if (ep != nullptr) {
        FWLOGINFO("bus node {:#x} disconnect from {:#x} due to no downstream and passive connection", node->get_id(),
                  upstream.current_bus_id);
        node->disconnect(upstream.current_bus_id);
      }
    }
  }
}

LIBATAPP_MACRO_NAMESPACE_END
