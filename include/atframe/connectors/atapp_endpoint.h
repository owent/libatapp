#ifndef LIBATAPP_ATAPP_CONNECTORS_ATAPP_ENDPOINT_H
#define LIBATAPP_ATAPP_CONNECTORS_ATAPP_ENDPOINT_H

#pragma once

#include <list>
#include <vector>

#include <config/atframe_utils_build_feature.h>
#include <config/compiler_features.h>

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include <time/time_utility.h>

#if defined(LIBATFRAME_UTILS_ENABLE_UNORDERED_MAP_SET) && LIBATFRAME_UTILS_ENABLE_UNORDERED_MAP_SET
#include <unordered_map>
#include <unordered_set>
#else
#include <map>
#include <set>
#endif

#include <atframe/atapp_conf.h>
#include <atframe/etcdcli/etcd_discovery.h>

namespace atapp {
    class app;
    class atapp_connection_handle;
    class atapp_endpoint;

    struct atapp_endpoint_bind_helper {
        // This API is used by inner system and will not be exported, do not call it directly
        static LIBATAPP_MACRO_API_SYMBOL_HIDDEN void unbind(atapp_connection_handle &handle, atapp_endpoint &connect);
        // This API is used by inner system and will not be exported, do not call it directly
        static LIBATAPP_MACRO_API_SYMBOL_HIDDEN void bind(atapp_connection_handle &handle, atapp_endpoint &connect);
    };

    class atapp_endpoint {
    public:
#if defined(UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES) && UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES
        using handle_set_t              = LIBATFRAME_UTILS_AUTO_SELETC_SET(atapp_connection_handle *);
        using handle_set_iterator       = handle_set_t::iterator;
        using handle_set_const_iterator = handle_set_t::const_iterator;
        using ptr_t                     = std::shared_ptr<atapp_endpoint>;
        using weak_ptr_t                = std::weak_ptr<atapp_endpoint>;
#else
        typedef LIBATFRAME_UTILS_AUTO_SELETC_SET(atapp_connection_handle *) handle_set_t;
        typedef handle_set_t::iterator handle_set_iterator;
        typedef handle_set_t::const_iterator handle_set_const_iterator;
        typedef std::shared_ptr<atapp_endpoint> ptr_t;
        typedef std::weak_ptr<atapp_endpoint> weak_ptr_t;
#endif

        struct pending_message_t {
            util::time::time_utility::raw_time_t expired_timepoint;
            int32_t type;
            uint64_t msg_sequence;
            std::vector<unsigned char> data;
            std::unique_ptr<atapp::protocol::atapp_metadata> metadata;
        };

        UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_endpoint)
        UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_endpoint)

    private:
        struct construct_helper_t {};

    public:
        LIBATAPP_MACRO_API atapp_endpoint(app &owner, construct_helper_t &helper);
        static LIBATAPP_MACRO_API ptr_t create(app &owner);
        LIBATAPP_MACRO_API ~atapp_endpoint();

        LIBATAPP_MACRO_API uint64_t get_id() const UTIL_CONFIG_NOEXCEPT;
        LIBATAPP_MACRO_API const std::string &get_name() const UTIL_CONFIG_NOEXCEPT;

        UTIL_FORCEINLINE bool has_connection_handle() const UTIL_CONFIG_NOEXCEPT { return !refer_connections_.empty(); }
        LIBATAPP_MACRO_API const etcd_discovery_node::ptr_t &get_discovery() const UTIL_CONFIG_NOEXCEPT;
        LIBATAPP_MACRO_API void update_discovery(const etcd_discovery_node::ptr_t &discovery) UTIL_CONFIG_NOEXCEPT;

        LIBATAPP_MACRO_API void add_connection_handle(atapp_connection_handle &handle);
        LIBATAPP_MACRO_API void remove_connection_handle(atapp_connection_handle &handle);
        LIBATAPP_MACRO_API atapp_connection_handle *get_ready_connection_handle() const UTIL_CONFIG_NOEXCEPT;

        LIBATAPP_MACRO_API int32_t push_forward_message(int32_t type, uint64_t &msg_sequence, const void *data, size_t data_size,
                                                        const atapp::protocol::atapp_metadata *metadata);

        LIBATAPP_MACRO_API int32_t retry_pending_messages(const util::time::time_utility::raw_time_t &tick_time, int32_t max_count = 0);
        LIBATAPP_MACRO_API void add_waker(util::time::time_utility::raw_time_t wakeup_time);

        UTIL_FORCEINLINE app * get_owner() const UTIL_CONFIG_NOEXCEPT { return owner_; }
    private:
        void reset();
        void cancel_pending_messages();

    private:
        bool closing_;
        app *owner_;
        util::time::time_utility::raw_time_t nearest_waker_;
        weak_ptr_t watcher_;
        handle_set_t refer_connections_;
        etcd_discovery_node::ptr_t discovery_;

        std::list<pending_message_t> pending_message_;
        size_t pending_message_size_;
#if defined(LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST) && LIBATAPP_ENABLE_CUSTOM_COUNT_FOR_STD_LIST
        size_t pending_message_count_;
#endif

        friend struct atapp_endpoint_bind_helper;
    };
} // namespace atapp

#endif