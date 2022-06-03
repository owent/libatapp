// Copyright 2021 atframework
// Created by owent on 2020-08-18

#pragma once

#include <gsl/select-gsl.h>

#include <config/atframe_utils_build_feature.h>
#include <config/compiler_features.h>

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include <random/random_generator.h>

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atframe/atapp_conf.h"

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
  using on_destroy_fn_type = std::function<void(etcd_discovery_node &)>;
  using ptr_t = std::shared_ptr<etcd_discovery_node>;

  UTIL_DESIGN_PATTERN_NOCOPYABLE(etcd_discovery_node)
  UTIL_DESIGN_PATTERN_NOMOVABLE(etcd_discovery_node)

 public:
  LIBATAPP_MACRO_API etcd_discovery_node();
  LIBATAPP_MACRO_API ~etcd_discovery_node();

  UTIL_FORCEINLINE const atapp::protocol::atapp_discovery &get_discovery_info() const { return node_info_; }
  LIBATAPP_MACRO_API void copy_from(const atapp::protocol::atapp_discovery &input);
  LIBATAPP_MACRO_API void copy_to(atapp::protocol::atapp_discovery &output) const;
  LIBATAPP_MACRO_API void copy_key_to(atapp::protocol::atapp_discovery &output) const;

  UTIL_FORCEINLINE const std::pair<uint64_t, uint64_t> &get_name_hash() const noexcept { return name_hash_; }

  UTIL_FORCEINLINE void set_private_data_ptr(void *input) noexcept { private_data_ptr_ = input; }
  UTIL_FORCEINLINE void *get_private_data_ptr() const noexcept { return private_data_ptr_; }
  UTIL_FORCEINLINE void set_private_data_u64(uint64_t input) noexcept { private_data_u64_ = input; }
  UTIL_FORCEINLINE uint64_t get_private_data_u64() const noexcept { return private_data_u64_; }
  UTIL_FORCEINLINE void set_private_data_i64(int64_t input) noexcept { private_data_i64_ = input; }
  UTIL_FORCEINLINE int64_t get_private_data_i64() const noexcept { return private_data_i64_; }
  UTIL_FORCEINLINE void set_private_data_uptr(uintptr_t input) noexcept { private_data_uptr_ = input; }
  UTIL_FORCEINLINE uintptr_t get_private_data_uptr() const noexcept { return private_data_uptr_; }
  UTIL_FORCEINLINE void set_private_data_iptr(intptr_t input) noexcept { private_data_iptr_ = input; }
  UTIL_FORCEINLINE intptr_t get_private_data_iptr() const noexcept { return private_data_iptr_; }

  LIBATAPP_MACRO_API void set_on_destroy(on_destroy_fn_type fn);
  LIBATAPP_MACRO_API const on_destroy_fn_type &get_on_destroy() const;
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
  on_destroy_fn_type on_destroy_fn_;
  mutable int32_t ingress_index_;
  mutable atapp::protocol::atapp_gateway ingress_for_listen_;
};

class etcd_discovery_set {
 public:
  using node_by_name_type = std::unordered_map<std::string, etcd_discovery_node::ptr_t>;
  using node_by_id_type = std::unordered_map<uint64_t, etcd_discovery_node::ptr_t>;
  using metadata_type = atapp::protocol::atapp_metadata;
  using ptr_t = std::shared_ptr<etcd_discovery_set>;

  struct node_hash_type {
    enum { HASH_POINT_PER_INS = 80 };

    etcd_discovery_node::ptr_t node;
    std::pair<uint64_t, uint64_t> hash_code;
  };

  struct metadata_hash_type {
    LIBATAPP_MACRO_API size_t operator()(const metadata_type &metadata) const noexcept;
  };

  struct metadata_equal_type {
    LIBATAPP_MACRO_API bool operator()(const metadata_type &l, const metadata_type &r) const noexcept;

    LIBATAPP_MACRO_API static bool filter(const metadata_type &rule, const metadata_type &metadata) noexcept;
  };

  UTIL_DESIGN_PATTERN_NOCOPYABLE(etcd_discovery_set)
  UTIL_DESIGN_PATTERN_NOMOVABLE(etcd_discovery_set)

 public:
  LIBATAPP_MACRO_API etcd_discovery_set();
  LIBATAPP_MACRO_API ~etcd_discovery_set();

  LIBATAPP_MACRO_API bool empty() const noexcept;
  LIBATAPP_MACRO_API size_t metadata_index_size() const noexcept;

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_id(uint64_t id) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_name(gsl::string_view name) const;

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(
      const void *buf, size_t bufsz, const metadata_type *metadata = nullptr) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(
      uint64_t key, const metadata_type *metadata = nullptr) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(
      int64_t key, const metadata_type *metadata = nullptr) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_consistent_hash(
      gsl::string_view key, const metadata_type *metadata = nullptr) const;

  template <class TKEY, class = typename std::enable_if<std::is_integral<TKEY>::value>::type>
  LIBATAPP_MACRO_API_HEAD_ONLY etcd_discovery_node::ptr_t get_node_by_consistent_hash(
      TKEY &&key, const metadata_type *metadata = nullptr) const {
    return get_node_by_consistent_hash(
        static_cast<typename std::conditional<std::is_unsigned<TKEY>::value, uint64_t, int64_t>::type>(key), metadata);
  }

  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_random(const metadata_type *metadata = nullptr) const;
  LIBATAPP_MACRO_API etcd_discovery_node::ptr_t get_node_by_round_robin(const metadata_type *metadata = nullptr) const;

  LIBATAPP_MACRO_API const std::vector<etcd_discovery_node::ptr_t> &get_sorted_nodes(
      const metadata_type *metadata = nullptr) const;
  LIBATAPP_MACRO_API std::vector<etcd_discovery_node::ptr_t>::const_iterator lower_bound_sorted_nodes(
      uint64_t id, gsl::string_view name, const metadata_type *metadata = nullptr) const;
  LIBATAPP_MACRO_API std::vector<etcd_discovery_node::ptr_t>::const_iterator upper_bound_sorted_nodes(
      uint64_t id, gsl::string_view name, const metadata_type *metadata = nullptr) const;

  LIBATAPP_MACRO_API void add_node(const etcd_discovery_node::ptr_t &node);
  LIBATAPP_MACRO_API void remove_node(const etcd_discovery_node::ptr_t &node);
  LIBATAPP_MACRO_API void remove_node(uint64_t id);
  LIBATAPP_MACRO_API void remove_node(gsl::string_view name);

 private:
  typedef struct {
    std::vector<node_hash_type> hashing_cache;
    std::vector<etcd_discovery_node::ptr_t> round_robin_cache;
    size_t round_robin_index;
  } index_cache_type;

  void rebuild_cache(index_cache_type &cache_set, const metadata_type *rule) const;
  void clear_cache(const metadata_type *rule) const;
  static void clear_cache(index_cache_type &cache_set);
  index_cache_type *get_index_cache(const metadata_type *metadata) const;
  index_cache_type *mutable_index_cache(const metadata_type *metadata) const;

 private:
  node_by_name_type node_by_name_;
  node_by_id_type node_by_id_;

  mutable util::random::xoshiro256_starstar random_generator_;
  mutable index_cache_type default_index_;
  mutable std::unordered_map<metadata_type, index_cache_type, metadata_hash_type, metadata_equal_type> metadata_index_;
};
}  // namespace atapp
