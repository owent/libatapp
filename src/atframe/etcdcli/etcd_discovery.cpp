// Copyright 2021 atframework
// Created by owent

#include <common/string_oprs.h>
#include <gsl/select-gsl.h>
#include <time/time_utility.h>

#include <algorithm/murmur_hash.h>

#include <atframe/etcdcli/etcd_discovery.h>

#include <algorithm>
#include <vector>

#ifdef max
#  undef max
#endif

namespace atapp {

namespace {
static std::pair<uint64_t, uint64_t> consistent_hash_calc(const void *buf, size_t bufsz, uint32_t seed) {
  std::pair<uint64_t, uint64_t> ret;
  uint64_t out[2] = {0};
  ::util::hash::murmur_hash3_x64_128(buf, static_cast<int>(bufsz), seed, out);
  ret.first = out[0];
  ret.second = out[1];

  return ret;
}

static void consistent_hash_combine(uint64_t &seed, uint64_t v) {
  // https://github.com/boostorg/container_hash/blob/develop/include/boost/container_hash/hash.hpp
  UTIL_CONFIG_CONSTEXPR const uint64_t m = (static_cast<uint64_t>(0xc6a4a793) << 32) + 0x5bd1e995;
  const int r = 47;

  v *= m;
  v ^= v >> r;
  v *= m;

  seed ^= v;
  seed *= m;

  // Completely arbitrary number, to prevent 0's
  // from hashing to 0.
  seed += 0xe6546b64;
}

static void consistent_hash_combine(const void *buf, size_t bufsz, std::pair<uint64_t, uint64_t> &seed) {
  std::pair<uint64_t, uint64_t> hash_value = consistent_hash_calc(buf, bufsz, static_cast<uint32_t>(seed.first));
  consistent_hash_combine(seed.first, hash_value.first);
  consistent_hash_combine(seed.second, hash_value.second);
}

static bool consistent_hash_compare_find(const etcd_discovery_set::node_hash_type &l,
                                         const std::pair<uint64_t, uint64_t> &r) {
  return l.hash_code < r;
}

static bool consistent_hash_compare_index(const etcd_discovery_set::node_hash_type &l,
                                          const etcd_discovery_set::node_hash_type &r) {
  if (l.hash_code != r.hash_code) {
    return l.hash_code < r.hash_code;
  }

  if (!l.node || !r.node) {
    return reinterpret_cast<uintptr_t>(l.node.get()) < reinterpret_cast<uintptr_t>(r.node.get());
  }

  if (l.node->get_discovery_info().id() != r.node->get_discovery_info().id()) {
    return l.node->get_discovery_info().id() < r.node->get_discovery_info().id();
  }

  if (l.node->get_name_hash() != r.node->get_name_hash()) {
    return l.node->get_name_hash() < r.node->get_name_hash();
  }

  return l.node->get_discovery_info().name() < r.node->get_discovery_info().name();
}

static bool round_robin_compare_index(const etcd_discovery_node::ptr_t &l, const etcd_discovery_node::ptr_t &r) {
  if (!l || !r) {
    return reinterpret_cast<uintptr_t>(l.get()) < reinterpret_cast<uintptr_t>(r.get());
  }

  if (l->get_discovery_info().id() != r->get_discovery_info().id()) {
    return l->get_discovery_info().id() < r->get_discovery_info().id();
  }

  if (l->get_name_hash() != r->get_name_hash()) {
    return l->get_name_hash() < r->get_name_hash();
  }

  return l->get_discovery_info().name() < r->get_discovery_info().name();
}

static void sort_string_map(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &input,
                            std::vector<std::pair<gsl::string_view, gsl::string_view>> &output) {
  output.clear();
  output.reserve(input.size());
  for (auto it = input.begin(); it != input.end(); ++it) {
    output.push_back(std::pair<gsl::string_view, gsl::string_view>(it->first, it->second));
  }

  std::sort(output.begin(), output.end(),
            [](const std::pair<gsl::string_view, gsl::string_view> &l,
               const std::pair<gsl::string_view, gsl::string_view> &r) { return l.first < r.first; });
}

struct lower_upper_bound_pred_t {
  uint64_t id;
  gsl::string_view name;
  std::pair<uint64_t, uint64_t> hash_code;
};

static bool lower_bound_compare_index(const etcd_discovery_node::ptr_t &l, const lower_upper_bound_pred_t &r) {
  if (!l) {
    return true;
  }

  if (l->get_discovery_info().id() != r.id) {
    return l->get_discovery_info().id() < r.id;
  }

  if (r.name.empty()) {
    return false;
  }

  if (l->get_name_hash() != r.hash_code) {
    return l->get_name_hash() < r.hash_code;
  }

  return UTIL_STRFUNC_STRNCMP(l->get_discovery_info().name().c_str(), r.name.data(), r.name.size()) < 0;
}

static bool upper_bound_compare_index(const lower_upper_bound_pred_t &l, const etcd_discovery_node::ptr_t &r) {
  if (!r) {
    return false;
  }

  if (l.id != r->get_discovery_info().id()) {
    return l.id < r->get_discovery_info().id();
  }

  if (l.name.empty()) {
    return true;
  }

  if (l.hash_code != r->get_name_hash()) {
    return l.hash_code < r->get_name_hash();
  }

  return UTIL_STRFUNC_STRNCMP(l.name.data(), r->get_discovery_info().name().c_str(), l.name.size()) < 0;
}

static const std::vector<etcd_discovery_node::ptr_t> &get_empty_discovery_set() {
  static std::vector<etcd_discovery_node::ptr_t> empty;
  return empty;
}
}  // namespace

LIBATAPP_MACRO_API etcd_discovery_node::etcd_discovery_node() : name_hash_(0, 0), ingress_index_(0) {
  private_data_ptr_ = nullptr;
  private_data_u64_ = 0;
  private_data_uptr_ = 0;
}

LIBATAPP_MACRO_API etcd_discovery_node::~etcd_discovery_node() {
  if (on_destroy_fn_) {
    on_destroy_fn_(*this);
  }
}

LIBATAPP_MACRO_API void etcd_discovery_node::copy_from(const atapp::protocol::atapp_discovery &input) {
  node_info_.CopyFrom(input);

  name_hash_ = consistent_hash_calc(input.name().c_str(), input.name().size(), LIBATAPP_MACRO_HASH_MAGIC_NUMBER);
}

LIBATAPP_MACRO_API void etcd_discovery_node::copy_to(atapp::protocol::atapp_discovery &output) const {
  output.CopyFrom(node_info_);
}

LIBATAPP_MACRO_API void etcd_discovery_node::copy_key_to(atapp::protocol::atapp_discovery &output) const {
  output.set_id(node_info_.id());
  output.set_name(node_info_.name());
  output.set_identity(node_info_.identity());
  output.set_hash_code(node_info_.hash_code());

  output.set_type_name(node_info_.type_name());
  output.set_type_id(node_info_.type_id());

  output.set_pid(node_info_.pid());
  output.set_hostname(node_info_.hostname());
}

LIBATAPP_MACRO_API void etcd_discovery_node::set_on_destroy(on_destroy_fn_type fn) { on_destroy_fn_ = fn; }

LIBATAPP_MACRO_API const etcd_discovery_node::on_destroy_fn_type &etcd_discovery_node::get_on_destroy() const {
  return on_destroy_fn_;
}

LIBATAPP_MACRO_API void etcd_discovery_node::reset_on_destroy() {
  on_destroy_fn_type empty;
  on_destroy_fn_.swap(empty);
}

LIBATAPP_MACRO_API const atapp::protocol::atapp_gateway &etcd_discovery_node::next_ingress_gateway() const {
  if (ingress_index_ < 0) {
    ingress_index_ = 0;
  }

  if (node_info_.gateways_size() > 0) {
    if (ingress_index_ >= node_info_.gateways_size()) {
      ingress_index_ %= node_info_.gateways_size();
    }
    return node_info_.gateways(ingress_index_++);
  }

  if (node_info_.listen_size() > 0) {
    if (ingress_index_ >= node_info_.listen_size()) {
      ingress_index_ %= node_info_.listen_size();
    }
    ingress_for_listen_.set_address(node_info_.listen(ingress_index_++));
    return ingress_for_listen_;
  }

  // if none of gateways or listen found, ingress_for_listen_ will always be empty
  return ingress_for_listen_;
}

LIBATAPP_MACRO_API int32_t etcd_discovery_node::get_ingress_size() const {
  if (node_info_.gateways_size() > 0) {
    return node_info_.gateways_size();
  }

  return node_info_.listen_size();
}

LIBATAPP_MACRO_API size_t
etcd_discovery_set::metadata_hash_type::operator()(const metadata_type &metadata) const noexcept {
  std::pair<uint64_t, uint64_t> hash_value{LIBATAPP_MACRO_HASH_MAGIC_NUMBER, 0};

  if (!metadata.namespace_name().empty()) {
    consistent_hash_combine(reinterpret_cast<const void *>(metadata.namespace_name().c_str()),
                            metadata.namespace_name().size(), hash_value);
  }

  if (!metadata.api_version().empty()) {
    consistent_hash_combine(reinterpret_cast<const void *>(metadata.api_version().c_str()),
                            metadata.api_version().size(), hash_value);
  }

  if (!metadata.kind().empty()) {
    consistent_hash_combine(reinterpret_cast<const void *>(metadata.kind().c_str()), metadata.kind().size(),
                            hash_value);
  }

  if (!metadata.group().empty()) {
    consistent_hash_combine(reinterpret_cast<const void *>(metadata.group().c_str()), metadata.group().size(),
                            hash_value);
  }

  if (!metadata.service_subset().empty()) {
    consistent_hash_combine(reinterpret_cast<const void *>(metadata.service_subset().c_str()),
                            metadata.service_subset().size(), hash_value);
  }

  std::vector<std::pair<gsl::string_view, gsl::string_view>> kvs;
  if (metadata.labels_size() > 0) {
    sort_string_map(metadata.labels(), kvs);
    for (auto &label : kvs) {
      consistent_hash_combine(reinterpret_cast<const void *>(label.second.data()), label.second.size(), hash_value);
    }
  }

  if (metadata.annotations_size() > 0) {
    sort_string_map(metadata.annotations(), kvs);
    for (auto &annotations : kvs) {
      consistent_hash_combine(reinterpret_cast<const void *>(annotations.second.data()), annotations.second.size(),
                              hash_value);
    }
  }

  return hash_value.first;
}

LIBATAPP_MACRO_API bool etcd_discovery_set::metadata_equal_type::operator()(const metadata_type &l,
                                                                            const metadata_type &r) const noexcept {
  if (&l == &r) {
    return true;
  }

  if (l.namespace_name().size() != r.namespace_name().size()) {
    return false;
  }

  if (l.api_version().size() != r.api_version().size()) {
    return false;
  }

  if (l.kind().size() != r.kind().size()) {
    return false;
  }

  if (l.group().size() != r.group().size()) {
    return false;
  }

  if (l.service_subset().size() != r.service_subset().size()) {
    return false;
  }

  if (l.labels_size() != r.labels_size()) {
    return false;
  }

  if (l.annotations_size() != r.annotations_size()) {
    return false;
  }

  if (l.namespace_name() != r.namespace_name()) {
    return false;
  }

  if (l.api_version() != r.api_version()) {
    return false;
  }

  if (l.kind() != r.kind()) {
    return false;
  }

  if (l.group() != r.group()) {
    return false;
  }

  if (l.service_subset() != r.service_subset()) {
    return false;
  }

  std::vector<std::pair<gsl::string_view, gsl::string_view>> kvs_l;
  std::vector<std::pair<gsl::string_view, gsl::string_view>> kvs_r;
  sort_string_map(l.labels(), kvs_l);
  sort_string_map(r.labels(), kvs_r);
  for (size_t i = 0; i < kvs_l.size() && i < kvs_r.size(); ++i) {
    if (kvs_l[i].first.size() != kvs_r[i].first.size() || kvs_l[i].second.size() != kvs_r[i].second.size()) {
      return false;
    }

    if (kvs_l[i].first != kvs_r[i].first || kvs_l[i].second != kvs_r[i].second) {
      return false;
    }
  }

  sort_string_map(l.annotations(), kvs_l);
  sort_string_map(r.annotations(), kvs_r);
  for (size_t i = 0; i < kvs_l.size() && i < kvs_r.size(); ++i) {
    if (kvs_l[i].first.size() != kvs_r[i].first.size() || kvs_l[i].second.size() != kvs_r[i].second.size()) {
      return false;
    }

    if (kvs_l[i].first != kvs_r[i].first || kvs_l[i].second != kvs_r[i].second) {
      return false;
    }
  }

  return true;
}

LIBATAPP_MACRO_API bool etcd_discovery_set::metadata_equal_type::filter(const metadata_type &rule,
                                                                        const metadata_type &metadata) noexcept {
  if (!rule.namespace_name().empty() && rule.namespace_name() != metadata.namespace_name()) {
    return false;
  }

  if (!rule.api_version().empty() && rule.api_version() != metadata.api_version()) {
    return false;
  }

  if (!rule.kind().empty() && rule.kind() != metadata.kind()) {
    return false;
  }

  if (!rule.group().empty() && rule.group() != metadata.group()) {
    return false;
  }

  if (!rule.service_subset().empty() && rule.service_subset() != metadata.service_subset()) {
    return false;
  }

  for (auto &label : rule.labels()) {
    if (label.second.empty()) {
      continue;
    }

    auto iter = metadata.labels().find(label.first);
    if (iter == metadata.labels().end()) {
      return false;
    }

    if (iter->second.size() != label.second.size()) {
      return false;
    }

    if (iter->second != label.second) {
      return false;
    }
  }

  for (auto &annotation : rule.annotations()) {
    if (annotation.second.empty()) {
      continue;
    }

    auto iter = metadata.annotations().find(annotation.first);
    if (iter == metadata.annotations().end()) {
      return false;
    }

    if (iter->second.size() != annotation.second.size()) {
      return false;
    }

    if (iter->second != annotation.second) {
      return false;
    }
  }

  return true;
}

LIBATAPP_MACRO_API etcd_discovery_set::etcd_discovery_set() {
  random_generator_.init_seed(
      static_cast<util::random::xoshiro256_starstar::result_type>(util::time::time_utility::get_now()));
  default_index_.round_robin_index = 0;
}

LIBATAPP_MACRO_API etcd_discovery_set::~etcd_discovery_set() {}

LIBATAPP_MACRO_API bool etcd_discovery_set::empty() const noexcept {
  return node_by_name_.empty() && node_by_id_.empty();
}

LIBATAPP_MACRO_API size_t etcd_discovery_set::metadata_index_size() const noexcept { return metadata_index_.size(); }

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_id(uint64_t id) const {
  node_by_id_type::const_iterator iter = node_by_id_.find(id);
  if (iter == node_by_id_.end()) {
    return nullptr;
  }

  return iter->second;
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_name(const std::string &name) const {
  node_by_name_type::const_iterator iter = node_by_name_.find(name);
  if (iter == node_by_name_.end()) {
    return nullptr;
  }

  return iter->second;
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(
    const void *buf, size_t bufsz, const metadata_type *metadata) const {
  index_cache_type *index_set = mutable_index_cache(metadata);
  UTIL_UNLIKELY_IF(nullptr == index_set) { return nullptr; }

  if (index_set->hashing_cache.empty()) {
    rebuild_cache(*index_set, metadata);

    if (index_set->hashing_cache.empty()) {
      return nullptr;
    }
  }

  std::pair<uint64_t, uint64_t> hash_key = consistent_hash_calc(buf, bufsz, LIBATAPP_MACRO_HASH_MAGIC_NUMBER);
  std::vector<node_hash_type>::const_iterator hash_iter = std::lower_bound(
      index_set->hashing_cache.begin(), index_set->hashing_cache.end(), hash_key, consistent_hash_compare_find);
  if (hash_iter == index_set->hashing_cache.end()) {
    return (*index_set->hashing_cache.begin()).node;
  }

  return (*hash_iter).node;
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(
    uint64_t key, const metadata_type *metadata) const {
  return get_node_by_consistent_hash(&key, sizeof(key), metadata);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(
    int64_t key, const metadata_type *metadata) const {
  return get_node_by_consistent_hash(&key, sizeof(key), metadata);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(
    gsl::string_view key, const metadata_type *metadata) const {
  return get_node_by_consistent_hash(key.data(), key.size(), metadata);
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_random(
    const metadata_type *metadata) const {
  index_cache_type *index_set = mutable_index_cache(metadata);
  UTIL_UNLIKELY_IF(nullptr == index_set) { return nullptr; }

  if (index_set->round_robin_cache.empty()) {
    rebuild_cache(*index_set, metadata);
    if (index_set->round_robin_cache.empty()) {
      return nullptr;
    }
  }

  if (index_set->round_robin_cache.empty()) {
    return nullptr;
  }

  return index_set->round_robin_cache[random_generator_.random_between<size_t>(0, index_set->round_robin_cache.size())];
}

LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_round_robin(
    const metadata_type *metadata) const {
  index_cache_type *index_set = mutable_index_cache(metadata);
  UTIL_UNLIKELY_IF(nullptr == index_set) { return nullptr; }

  if (index_set->round_robin_cache.empty()) {
    rebuild_cache(*index_set, metadata);
    if (index_set->round_robin_cache.empty()) {
      return nullptr;
    }
  }

  if (index_set->round_robin_cache.empty()) {
    return nullptr;
  }

  if (index_set->round_robin_index >= index_set->round_robin_cache.size()) {
    index_set->round_robin_index %= index_set->round_robin_cache.size();
  }

  return index_set->round_robin_cache[index_set->round_robin_index++];
}

LIBATAPP_MACRO_API const std::vector<etcd_discovery_node::ptr_t> &etcd_discovery_set::get_sorted_nodes(
    const metadata_type *metadata) const {
  index_cache_type *index_set = mutable_index_cache(metadata);
  UTIL_UNLIKELY_IF(nullptr == index_set) { return get_empty_discovery_set(); }

  if (index_set->round_robin_cache.empty()) {
    rebuild_cache(*index_set, metadata);
  }

  return index_set->round_robin_cache;
}

LIBATAPP_MACRO_API std::vector<etcd_discovery_node::ptr_t>::const_iterator etcd_discovery_set::lower_bound_sorted_nodes(
    uint64_t id, gsl::string_view name, const metadata_type *metadata) const {
  const std::vector<etcd_discovery_node::ptr_t> &container = get_sorted_nodes(metadata);

  lower_upper_bound_pred_t pred_val;
  pred_val.id = id;
  pred_val.name = name;
  if (!name.empty()) {
    pred_val.hash_code = consistent_hash_calc(name.data(), name.size(), LIBATAPP_MACRO_HASH_MAGIC_NUMBER);
  } else {
    pred_val.hash_code = std::pair<uint64_t, uint64_t>(0, 0);
  }

  return std::lower_bound(container.begin(), container.end(), pred_val, lower_bound_compare_index);
}

LIBATAPP_MACRO_API std::vector<etcd_discovery_node::ptr_t>::const_iterator etcd_discovery_set::upper_bound_sorted_nodes(
    uint64_t id, gsl::string_view name, const metadata_type *metadata) const {
  const std::vector<etcd_discovery_node::ptr_t> &container = get_sorted_nodes(metadata);

  lower_upper_bound_pred_t pred_val;
  pred_val.id = id;
  pred_val.name = name;
  if (!name.empty()) {
    pred_val.hash_code = consistent_hash_calc(name.data(), name.size(), LIBATAPP_MACRO_HASH_MAGIC_NUMBER);
  } else {
    pred_val.hash_code = std::pair<uint64_t, uint64_t>(0, 0);
  }

  return std::upper_bound(container.begin(), container.end(), pred_val, upper_bound_compare_index);
}

LIBATAPP_MACRO_API void etcd_discovery_set::add_node(const etcd_discovery_node::ptr_t &node) {
  if (!node) {
    return;
  }

  bool has_insert = false;
  std::string old_name;
  uint64_t old_id = 0;

  // Insert into id index if id != 0
  if (0 != node->get_discovery_info().id()) {
    node_by_id_type::iterator iter_id = node_by_id_.find(node->get_discovery_info().id());
    if (iter_id == node_by_id_.end()) {
      node_by_id_[node->get_discovery_info().id()] = node;
      has_insert = true;
    } else if (iter_id->second != node) {
      // name change and remove node of old name
      if (iter_id->second->get_discovery_info().name() != node->get_discovery_info().name()) {
        old_name = iter_id->second->get_discovery_info().name();
      }

      // Remove old first, because directly change value of shared_ptr is not thread-safe
      iter_id->second.reset();
      iter_id->second = node;
      has_insert = true;
    }
  }

  // Insert into name index if name().empty() != true
  if (!node->get_discovery_info().name().empty()) {
    node_by_name_type::iterator iter_name = node_by_name_.find(node->get_discovery_info().name());
    if (iter_name == node_by_name_.end()) {
      node_by_name_[node->get_discovery_info().name()] = node;
      has_insert = true;
    } else if (iter_name->second != node) {
      // id change and remove node of old id
      if (iter_name->second->get_discovery_info().id() != node->get_discovery_info().id()) {
        old_id = iter_name->second->get_discovery_info().id();
      }

      // Remove old first, because directly change value of shared_ptr is not thread-safe
      iter_name->second.reset();
      iter_name->second = node;
      has_insert = true;
    }
  }

  if (has_insert) {
    if (0 != old_id) {
      node_by_id_type::iterator iter_id = node_by_id_.find(old_id);
      if (iter_id != node_by_id_.end() && iter_id->second != node) {
        node_by_id_.erase(iter_id);
      }
    }

    if (!old_name.empty()) {
      node_by_name_type::iterator iter_name = node_by_name_.find(old_name);
      if (iter_name != node_by_name_.end() && iter_name->second != node) {
        node_by_name_.erase(iter_name);
      }
    }

    if (node->get_discovery_info().has_metadata()) {
      clear_cache(node->get_discovery_info().metadata());
    }
  }
}

LIBATAPP_MACRO_API void etcd_discovery_set::remove_node(const etcd_discovery_node::ptr_t &node) {
  if (!node) {
    return;
  }

  bool has_cleanup = false;
  if (!node->get_discovery_info().name().empty()) {
    node_by_name_type::iterator iter_name = node_by_name_.find(node->get_discovery_info().name());
    if (iter_name != node_by_name_.end() && iter_name->second == node) {
      node_by_name_.erase(iter_name);
      has_cleanup = true;
    }
  }

  if (0 != node->get_discovery_info().id()) {
    node_by_id_type::iterator iter_id = node_by_id_.find(node->get_discovery_info().id());
    if (iter_id != node_by_id_.end() && node == iter_id->second) {
      node_by_id_.erase(iter_id);
      has_cleanup = true;
    }
  }

  if (has_cleanup) {
    if (node->get_discovery_info().has_metadata()) {
      clear_cache(node->get_discovery_info().metadata());
    }
  }
}

LIBATAPP_MACRO_API void etcd_discovery_set::remove_node(uint64_t id) {
  node_by_id_type::iterator iter_id = node_by_id_.find(id);
  if (iter_id == node_by_id_.end()) {
    return;
  }

  if (iter_id->second && !iter_id->second->get_discovery_info().name().empty()) {
    node_by_name_type::iterator iter_name = node_by_name_.find(iter_id->second->get_discovery_info().name());
    if (iter_name != node_by_name_.end() && iter_name->second == iter_id->second) {
      node_by_name_.erase(iter_name);
    }
  }

  if (iter_id->second->get_discovery_info().has_metadata()) {
    clear_cache(iter_id->second->get_discovery_info().metadata());
  }

  node_by_id_.erase(iter_id);
}

LIBATAPP_MACRO_API void etcd_discovery_set::remove_node(const std::string &name) {
  node_by_name_type::iterator iter_name = node_by_name_.find(name);
  if (iter_name == node_by_name_.end()) {
    return;
  }

  if (iter_name->second && 0 != iter_name->second->get_discovery_info().id()) {
    node_by_id_type::iterator iter_id = node_by_id_.find(iter_name->second->get_discovery_info().id());
    if (iter_id != node_by_id_.end() && iter_name->second == iter_id->second) {
      node_by_id_.erase(iter_id);
    }
  }

  if (iter_name->second->get_discovery_info().has_metadata()) {
    clear_cache(iter_name->second->get_discovery_info().metadata());
  }

  node_by_name_.erase(iter_name);
}

void etcd_discovery_set::rebuild_cache(index_cache_type &cache_set, const metadata_type *rule) const {
  using std::max;

  if (!cache_set.hashing_cache.empty()) {
    return;
  }

  if (node_by_id_.empty() && node_by_name_.empty()) {
    return;
  }

  clear_cache(cache_set);

  if (&default_index_ == &cache_set) {
    cache_set.round_robin_cache.reserve(max(node_by_id_.size(), node_by_name_.size()));
    cache_set.hashing_cache.reserve(max(node_by_id_.size(), node_by_name_.size()) * node_hash_type::HASH_POINT_PER_INS);
  }

  for (node_by_id_type::const_iterator iter = node_by_id_.begin(); iter != node_by_id_.end(); ++iter) {
    if (nullptr != rule && !metadata_equal_type::filter(*rule, iter->second->get_discovery_info().metadata())) {
      continue;
    }

    cache_set.round_robin_cache.push_back(iter->second);

    for (size_t i = 0; i < node_hash_type::HASH_POINT_PER_INS / 2; ++i) {
      node_hash_type hash_node;
      hash_node.node = iter->second;
      uint64_t key = iter->second->get_discovery_info().id();
      hash_node.hash_code = consistent_hash_calc(&key, sizeof(key), static_cast<uint32_t>(i));

      cache_set.hashing_cache.push_back(hash_node);
    }
  }

  for (node_by_name_type::const_iterator iter = node_by_name_.begin(); iter != node_by_name_.end(); ++iter) {
    if (nullptr != rule && !metadata_equal_type::filter(*rule, iter->second->get_discovery_info().metadata())) {
      continue;
    }

    // If already pushed by id, skip round robin cache
    if (0 == iter->second->get_discovery_info().id()) {
      cache_set.round_robin_cache.push_back(iter->second);
    }

    for (size_t i = 0; i < node_hash_type::HASH_POINT_PER_INS / 2; ++i) {
      node_hash_type hash_node;
      hash_node.node = iter->second;
      const std::string &name = iter->second->get_discovery_info().name();
      hash_node.hash_code = consistent_hash_calc(name.c_str(), name.size(), static_cast<uint32_t>(i));

      cache_set.hashing_cache.push_back(hash_node);
    }
  }

  std::sort(cache_set.round_robin_cache.begin(), cache_set.round_robin_cache.end(), round_robin_compare_index);
  cache_set.round_robin_index = 0;
  std::sort(cache_set.hashing_cache.begin(), cache_set.hashing_cache.end(), consistent_hash_compare_index);
}

void etcd_discovery_set::clear_cache(const metadata_type &metadata) const {
  clear_cache(default_index_);

  std::vector<const metadata_type *> pending_to_delete;
  for (auto &metadata_index : metadata_index_) {
    if (metadata_equal_type::filter(metadata_index.first, metadata)) {
      pending_to_delete.push_back(&metadata_index.first);
    }
  }

  for (auto &pending_to_delete_index : pending_to_delete) {
    metadata_index_.erase(*pending_to_delete_index);
  }
}

void etcd_discovery_set::clear_cache(index_cache_type &cache_set) {
  cache_set.hashing_cache.clear();
  cache_set.round_robin_cache.clear();
}

etcd_discovery_set::index_cache_type *etcd_discovery_set::get_index_cache(
    const etcd_discovery_set::metadata_type *metadata) const {
  if (nullptr == metadata) {
    return &default_index_;
  }

  auto iter = metadata_index_.find(*metadata);
  if (iter == metadata_index_.end()) {
    return nullptr;
  }

  return &iter->second;
}

etcd_discovery_set::index_cache_type *etcd_discovery_set::mutable_index_cache(
    const etcd_discovery_set::metadata_type *metadata) const {
  auto ret = get_index_cache(metadata);
  if (nullptr != ret) {
    return ret;
  }

  ret = &metadata_index_[*metadata];
  return ret;
}

}  // namespace atapp
