/**
 * etcd_def.h
 *
 *  Created on: 2020-08-18
 *      Author: owent
 *
 *  Released under the MIT license
 */

#ifndef LIBATAPP_ETCDCLI_ETCD_DISCOVERY_H
#define LIBATAPP_ETCDCLI_ETCD_DISCOVERY_H

#pragma once

#include <config/atframe_utils_build_feature.h>
#include <config/compiler_features.h>

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include <random/random_generator.h>

#include <atframe/atapp_conf.h>

namespace atapp {
struct LIBATAPP_MACRO_API_HEAD_ONLY etcd_discovery_action_t {
  enum type {
    EN_NAT_UNKNOWN = 0,
    EN_NAT_PUT,
    EN_NAT_DELETE,
  };
};

class etcd_discovery_node {
 public:
#if defined(UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES) && UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES
  using on_destroy_fn_t = std::function<void(etcd_discovery_node &)>;
  using ptr_t = std::shared_ptr<etcd_discovery_node>;
#else
  typedef std::function<void(etcd_discovery_node &)> on_destroy_fn_t;
  typedef std::shared_ptr<etcd_discovery_node> ptr_t;
#endif

  UTIL_DESIGN_PATTERN_NOCOPYABLE(etcd_discovery_node)
  UTIL_DESIGN_PATTERN_NOMOVABLE(etcd_discovery_node)

 public:
  LIBATAPP_MACRO_API etcd_discovery_node();
  LIBATAPP_MACRO_API ~etcd_discovery_node();

  UTIL_FORCEINLINE const atapp::protocol::atapp_discovery &get_discovery_info() const { return node_info_; }
  LIBATAPP_MACRO_API void copy_from(const atapp::protocol::atapp_discovery &input);

  UTIL_FORCEINLINE const std::pair<uint64_t, uint64_t> &get_name_hash() const { return name_hash_; }

  UTIL_FORCEINLINE void set_private_data_ptr(void *input) { private_data_ptr_ = input; }
  UTIL_FORCEINLINE void *get_private_data_ptr() const { return private_data_ptr_; }
  UTIL_FORCEINLINE void set_private_data_u64(uint64_t input) { private_data_u64_ = input; }
  UTIL_FORCEINLINE uint64_t get_private_data_u64() const { return private_data_u64_; }
  UTIL_FORCEINLINE void set_private_data_i64(int64_t input) { private_data_i64_ = input; }
  UTIL_FORCEINLINE int64_t get_private_data_i64() const { return private_data_i64_; }
  UTIL_FORCEINLINE void set_private_data_uptr(uintptr_t input) { private_data_uptr_ = input; }
  UTIL_FORCEINLINE uintptr_t get_private_data_uptr() const { return private_data_uptr_; }
  UTIL_FORCEINLINE void set_private_data_iptr(intptr_t input) { private_data_iptr_ = input; }
  UTIL_FORCEINLINE intptr_t get_private_data_iptr() const { return private_data_iptr_; }

  LIBATAPP_MACRO_API void set_on_destroy(on_destroy_fn_t fn);
  LIBATAPP_MACRO_API const on_destroy_fn_t &get_on_destroy() const;
  LIBATAPP_MACRO_API void reset_on_destroy();

  LIBATAPP_MACRO_API const atapp::protocol::atapp_gateway &next_ingress_gateway() const;
  LIBATAPP_MACRO_API int32_t get_ingress_size() const;

 private:
  atapp::protocol::atapp_discovery node_info_;
  std::pair<uint64_t, uint64_t> name_hash_;
  union {
    void *private_data_ptr_;
    uint64_t private_data_u64_;
    int64_t private_data_i64_;
    uintptr_t private_data_uptr_;
    intptr_t private_data_iptr_;
  };
  on_destroy_fn_t on_destroy_fn_;
  mutable int32_t ingress_index_;
  mutable atapp::protocol::atapp_gateway ingress_for_listen_;
};

class etcd_discovery_set {
 public:
#if defined(UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES) && UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES
  using node_by_name_t = LIBATFRAME_UTILS_AUTO_SELETC_MAP(std::string, etcd_discovery_node::ptr_t);
  using node_by_id_t = LIBATFRAME_UTILS_AUTO_SELETC_MAP(uint64_t, etcd_discovery_node::ptr_t);
  using ptr_t = std::shared_ptr<etcd_discovery_set>;
#else
  typedef LIBATFRAME_UTILS_AUTO_SELETC_MAP(std::string, etcd_discovery_node::ptr_t) node_by_name_t;
  typedef LIBATFRAME_UTILS_AUTO_SELETC_MAP(uint64_t, etcd_discovery_node::ptr_t) node_by_id_t;
  typedef std::shared_ptr<etcd_discovery_set> ptr_t;
#endif

  struct node_hash_t {
    enum { HASH_POINT_PER_INS = 80 };

    etcd_discovery_node::ptr_t node;
    std::pair<uint64_t, uint64_t> hash_code;
  };

  UTIL_DESIGN_PATTERN_NOCOPYABLE(etcd_discovery_set)
  UTIL_DESIGN_PATTERN_NOMOVABLE(etcd_discovery_set)
 public:
  LIBATAPP_MACRO_API etcd_discovery_set();
  LIBATAPP_MACRO_API ~etcd_discovery_set();

  LIBATAPP_MACRO_API bool empty() const;

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_id(uint64_t id) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_name(const std::string &name) const;

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(const void *buf, size_t bufsz) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(uint64_t key) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(int64_t key) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(const std::string &key) const;

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_random() const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_round_robin() const;

  LIBATAPP_MACRO_API const std::vector<etcd_discovery_node::ptr_t> &get_sorted_nodes() const;
  LIBATAPP_MACRO_API std::vector<etcd_discovery_node::ptr_t>::const_iterator lower_bound_sorted_nodes(
      uint64_t id, const std::string &name) const;
  LIBATAPP_MACRO_API std::vector<etcd_discovery_node::ptr_t>::const_iterator upper_bound_sorted_nodes(
      uint64_t id, const std::string &name) const;

  LIBATAPP_MACRO_API void add_node(const etcd_discovery_node::ptr_t &node);
  LIBATAPP_MACRO_API void remove_node(const etcd_discovery_node::ptr_t &node);
  LIBATAPP_MACRO_API void remove_node(uint64_t id);
  LIBATAPP_MACRO_API void remove_node(const std::string &name);

 private:
  void rebuild_cache() const;
  void clear_cache() const;

 private:
  node_by_name_t node_by_name_;
  node_by_id_t node_by_id_;

  mutable std::vector<node_hash_t> hashing_cache_;
  mutable std::vector<etcd_discovery_node::ptr_t> round_robin_cache_;
  mutable util::random::xoshiro256_starstar random_generator_;
  mutable size_t round_robin_index_;
};
}  // namespace atapp

#endif
