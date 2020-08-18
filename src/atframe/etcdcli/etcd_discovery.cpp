#include <algorithm>

#include <time/time_utility.h>

#include <algorithm/murmur_hash.h>

#include <atframe/etcdcli/etcd_discovery.h>

#ifdef max
#undef max
#endif

namespace atapp {

    static std::pair<uint64_t, uint64_t> consistent_hash_calc(const void* buf, size_t bufsz, uint32_t seed) {
        std::pair<uint64_t, uint64_t> ret;
        uint64_t out[2] = {0};
        ::util::hash::murmur_hash3_x64_128(buf, static_cast<int>(bufsz), seed, out);
        ret.first = out[0];
        ret.second = out[1];

        return ret;
    }

    static bool consistent_hash_compare_find(const etcd_discovery_set::node_hash_t& l, const std::pair<uint64_t, uint64_t>& r) {
        return l.hash_code < r;
    }

    static bool consistent_hash_compare_index(const etcd_discovery_set::node_hash_t& l, const etcd_discovery_set::node_hash_t& r) {
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

    static bool round_robin_compare_index(const etcd_discovery_node::ptr_t& l, const etcd_discovery_node::ptr_t& r) {
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

    LIBATAPP_MACRO_API etcd_discovery_node::etcd_discovery_node(): name_hash_(0, 0) {
        private_data_ptr_ = NULL;
        private_data_u64_ = 0;
        private_data_uptr_ = 0;
    }

    LIBATAPP_MACRO_API etcd_discovery_node::~etcd_discovery_node() {
        if (on_destroy_fn_) {
            on_destroy_fn_(*this);
        }
    }

    LIBATAPP_MACRO_API void etcd_discovery_node::copy_from(const atapp::protocol::atapp_discovery& input) {
        node_info_.CopyFrom(input);

        name_hash_ = consistent_hash_calc(input.name().c_str(), input.name().size(), 0);
    }

    LIBATAPP_MACRO_API void etcd_discovery_node::set_on_destroy(on_destroy_fn_t fn) {
        on_destroy_fn_ = fn;
    }

    LIBATAPP_MACRO_API const etcd_discovery_node::on_destroy_fn_t& etcd_discovery_node::get_on_destroy() const {
        return on_destroy_fn_;
    }

    LIBATAPP_MACRO_API etcd_discovery_set::etcd_discovery_set() {
        random_generator_.init_seed(static_cast<util::random::xoshiro256_starstar::result_type>(util::time::time_utility::get_now()));
        round_robin_index_ = 0;
    }

    LIBATAPP_MACRO_API etcd_discovery_set::~etcd_discovery_set() { }


    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_id(uint64_t id) const {
        node_by_id_t::const_iterator iter = node_by_id_.find(id);
        if (iter == node_by_id_.end()) {
            return NULL;
        }

        return iter->second;
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_name(const std::string& name) const {
        node_by_name_t::const_iterator iter = node_by_name_.find(name);
        if (iter == node_by_name_.end()) {
            return NULL;
        }

        return iter->second;
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(const void* buf, size_t bufsz) const {
        if (hashing_cache_.empty()) {
            rebuild_cache();
        }

        std::pair<uint64_t, uint64_t> hash_key = consistent_hash_calc(buf, bufsz, 0);
        std::vector<node_hash_t>::const_iterator hash_iter = std::lower_bound(hashing_cache_.begin(), hashing_cache_.end(), hash_key, consistent_hash_compare_find);
        if (hash_iter == hashing_cache_.end()) {
            return (*hashing_cache_.begin()).node;
        }

        return (*hash_iter).node;
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(uint64_t key) const {
        return get_node_by_consistent_hash(&key, sizeof(key));
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(int64_t key) const {
        return get_node_by_consistent_hash(&key, sizeof(key));
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_consistent_hash(const std::string& key) const {
        return get_node_by_consistent_hash(key.c_str(), key.size());
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_random() const {
        if (round_robin_cache_.empty()) {
            rebuild_cache();
        }

        if (round_robin_cache_.empty()) {
            return NULL;
        }

        return round_robin_cache_[random_generator_.random_between<size_t>(0, round_robin_cache_.size())];
    }

    LIBATAPP_MACRO_API etcd_discovery_node::ptr_t etcd_discovery_set::get_node_by_round_robin() const {
        if (round_robin_cache_.empty()) {
            rebuild_cache();
        }

        if (round_robin_cache_.empty()) {
            return NULL;
        }

        if (round_robin_index_ > round_robin_cache_.size()) {
            round_robin_index_ %= round_robin_cache_.size();
        }

        return round_robin_cache_[round_robin_index_ ++];
    }

    LIBATAPP_MACRO_API void etcd_discovery_set::add_node(const etcd_discovery_node::ptr_t& node) {
        if (!node) {
            return;
        }

        bool has_insert = false;
        std::string old_name;
        uint64_t old_id = 0;

        // Insert into id index if id != 0
        if (0 != node->get_discovery_info().id()) {
            node_by_id_t::iterator iter_id = node_by_id_.find(node->get_discovery_info().id());
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
            node_by_name_t::iterator iter_name = node_by_name_.find(node->get_discovery_info().name());
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
                node_by_id_t::iterator iter_id = node_by_id_.find(old_id);
                if (iter_id != node_by_id_.end()) {
                    node_by_id_.erase(iter_id);
                }
            }

            if (!old_name.empty()) {
                node_by_name_t::iterator iter_name = node_by_name_.find(old_name);
                if (iter_name != node_by_name_.end()) {
                    node_by_name_.erase(iter_name);
                }
            }

            clear_cache();
        }
    }

    LIBATAPP_MACRO_API void etcd_discovery_set::remove_node(const etcd_discovery_node::ptr_t& node) {
        if (!node) {
            return;
        }

        bool has_cleanup = false;
        if (!node->get_discovery_info().name().empty()) {
            node_by_name_t::iterator iter_name = node_by_name_.find(node->get_discovery_info().name());
            if (iter_name != node_by_name_.end() && iter_name->second == node) {
                node_by_name_.erase(iter_name);
                has_cleanup = true;
            }
        }

        if (0 != node->get_discovery_info().id()) {
            node_by_id_t::iterator iter_id = node_by_id_.find(node->get_discovery_info().id());
            if (iter_id != node_by_id_.end() && node == iter_id->second) {
                node_by_id_.erase(iter_id);
                has_cleanup = true;
            }
        }

        if (has_cleanup) {
            clear_cache();
        }
    }

    LIBATAPP_MACRO_API void etcd_discovery_set::remove_node(uint64_t id) {
        node_by_id_t::iterator iter_id = node_by_id_.find(id);
        if (iter_id == node_by_id_.end()) {
            return;
        }

        if (iter_id->second && !iter_id->second->get_discovery_info().name().empty()) {
            node_by_name_t::iterator iter_name = node_by_name_.find(iter_id->second->get_discovery_info().name());
            if (iter_name != node_by_name_.end() && iter_name->second == iter_id->second) {
                node_by_name_.erase(iter_name);
            }
        }

        node_by_id_.erase(iter_id);

        clear_cache();
    }

    LIBATAPP_MACRO_API void etcd_discovery_set::remove_node(const std::string& name) {
        node_by_name_t::iterator iter_name = node_by_name_.find(name);
        if (iter_name == node_by_name_.end()) {
            return;
        }

        if (iter_name->second && 0 != iter_name->second->get_discovery_info().id()) {
            node_by_id_t::iterator iter_id = node_by_id_.find(iter_name->second->get_discovery_info().id());
            if (iter_id != node_by_id_.end() && iter_name->second == iter_id->second) {
                node_by_id_.erase(iter_id);
            }
        }

        node_by_name_.erase(iter_name);

        clear_cache();
    }

    void etcd_discovery_set::rebuild_cache() const {
        using std::max;

        if (!hashing_cache_.empty()) {
            return;
        }

        if (node_by_id_.empty() && node_by_name_.empty()) {
            return;
        }

        clear_cache();

        round_robin_cache_.reserve(max(node_by_id_.size(), node_by_name_.size()));
        hashing_cache_.reserve(max(node_by_id_.size(), node_by_name_.size()) * node_hash_t::HASH_POINT_PER_INS);


        for(node_by_id_t::const_iterator iter = node_by_id_.begin(); iter != node_by_id_.end(); ++ iter) {
            round_robin_cache_.push_back(iter->second);

            for (size_t i = 0; i < node_hash_t::HASH_POINT_PER_INS / 2; ++ i) {
                node_hash_t hash_node;
                hash_node.node = iter->second;
                uint64_t key = iter->second->get_discovery_info().id();
                hash_node.hash_code = consistent_hash_calc(&key, sizeof(key), static_cast<uint32_t>(i));

                hashing_cache_.push_back(hash_node);
            }
        }

        for(node_by_name_t::const_iterator iter = node_by_name_.begin(); iter != node_by_name_.end(); ++ iter) {
            // If already pushed by id, skip round robin cache
            if (0 == iter->second->get_discovery_info().id()) {
                round_robin_cache_.push_back(iter->second);
            }

            for (size_t i = 0; i < node_hash_t::HASH_POINT_PER_INS / 2; ++ i) {
                node_hash_t hash_node;
                hash_node.node = iter->second;
                const std::string& name = iter->second->get_discovery_info().name();
                hash_node.hash_code = consistent_hash_calc(name.c_str(), name.size(), static_cast<uint32_t>(i));

                hashing_cache_.push_back(hash_node);
            }
        }

        std::sort(round_robin_cache_.begin(), round_robin_cache_.end(), round_robin_compare_index);
        std::sort(hashing_cache_.begin(), hashing_cache_.end(), consistent_hash_compare_index);
    }

    void etcd_discovery_set::clear_cache() const {
        hashing_cache_.clear();
        round_robin_cache_.clear();
    }
}
