#ifndef LIBATAPP_ATAPP_CONNECTORS_ATAPP_CONNECTOR_IMPL_H
#define LIBATAPP_ATAPP_CONNECTORS_ATAPP_CONNECTOR_IMPL_H

#pragma once

#include <list>
#include <string>

#include <config/compile_optimize.h>
#include <config/compiler_features.h>

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>
#include <std/smart_ptr.h>

#include <common/demangle.h>

#include <detail/buffer.h>
#include <detail/libatbus_channel_export.h>

#include <atframe/atapp_config.h>
#include <atframe/etcdcli/etcd_discovery.h>

#include "atapp_endpoint.h"

namespace atapp {

    class atapp_connector_impl;

    struct atapp_connector_bind_helper {
        // This API is used by inner system and will not be exported, do not call it directly
        static LIBATAPP_MACRO_API_SYMBOL_HIDDEN void unbind(atapp_connection_handle &handle, atapp_connector_impl &connect);
        // This API is used by inner system and will not be exported, do not call it directly
        static LIBATAPP_MACRO_API_SYMBOL_HIDDEN void bind(atapp_connection_handle &handle, atapp_connector_impl &connect);
    };

    class atapp_connection_handle {
    public:
#if defined(UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES) && UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES
        using ptr_t           = std::shared_ptr<atapp_connection_handle>;
        using weak_ptr_t      = std::weak_ptr<atapp_connection_handle>;
        using on_destroy_fn_t = std::function<void(atapp_connection_handle &)>;
#else
        typedef std::shared_ptr<atapp_connection_handle> ptr_t;
        typedef std::weak_ptr<atapp_connection_handle> weak_ptr_t;
        typedef std::function<void(atapp_connection_handle &)> on_destroy_fn_t;
#endif

        struct LIBATAPP_MACRO_API_HEAD_ONLY flags_t {
            enum type {
                EN_ACH_NONE    = 0x00,
                EN_ACH_CLOSING = 0x01,
                EN_ACH_READY   = 0x02,
            };
        };

        UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connection_handle)
        UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connection_handle)

    public:
        LIBATAPP_MACRO_API atapp_connection_handle();
        LIBATAPP_MACRO_API ~atapp_connection_handle();
        LIBATAPP_MACRO_API void close();
        LIBATAPP_MACRO_API bool is_closing() const UTIL_CONFIG_NOEXCEPT;
        LIBATAPP_MACRO_API void set_ready() UTIL_CONFIG_NOEXCEPT;
        LIBATAPP_MACRO_API bool is_ready() const UTIL_CONFIG_NOEXCEPT;

        UTIL_FORCEINLINE void set_private_data_ptr(void *input) UTIL_CONFIG_NOEXCEPT { private_data_ptr_ = input; }
        UTIL_FORCEINLINE void *get_private_data_ptr() const UTIL_CONFIG_NOEXCEPT { return private_data_ptr_; }
        UTIL_FORCEINLINE void set_private_data_u64(uint64_t input) UTIL_CONFIG_NOEXCEPT { private_data_u64_ = input; }
        UTIL_FORCEINLINE uint64_t get_private_data_u64() const UTIL_CONFIG_NOEXCEPT { return private_data_u64_; }
        UTIL_FORCEINLINE void set_private_data_i64(int64_t input) UTIL_CONFIG_NOEXCEPT { private_data_i64_ = input; }
        UTIL_FORCEINLINE int64_t get_private_data_i64() const UTIL_CONFIG_NOEXCEPT { return private_data_i64_; }
        UTIL_FORCEINLINE void set_private_data_uptr(uintptr_t input) UTIL_CONFIG_NOEXCEPT { private_data_uptr_ = input; }
        UTIL_FORCEINLINE uintptr_t get_private_data_uptr() const UTIL_CONFIG_NOEXCEPT { return private_data_uptr_; }
        UTIL_FORCEINLINE void set_private_data_iptr(intptr_t input) UTIL_CONFIG_NOEXCEPT { private_data_iptr_ = input; }
        UTIL_FORCEINLINE intptr_t get_private_data_iptr() const UTIL_CONFIG_NOEXCEPT { return private_data_iptr_; }

        LIBATAPP_MACRO_API void set_on_destroy(on_destroy_fn_t fn);
        LIBATAPP_MACRO_API const on_destroy_fn_t &get_on_destroy() const;
        LIBATAPP_MACRO_API void reset_on_destroy();

        UTIL_FORCEINLINE atapp_connector_impl *get_connector() const UTIL_CONFIG_NOEXCEPT { return connector_; }
        UTIL_FORCEINLINE atapp_endpoint *get_endpoint() const UTIL_CONFIG_NOEXCEPT { return endpiont_; }

    private:
        uint32_t flags_;
        union {
            void *private_data_ptr_;
            uint64_t private_data_u64_;
            int64_t private_data_i64_;
            uintptr_t private_data_uptr_;
            intptr_t private_data_iptr_;
        };
        atapp_connector_impl *connector_;
        atapp_endpoint *endpiont_;
        on_destroy_fn_t on_destroy_fn_;

        friend struct atapp_connector_bind_helper;
        friend struct atapp_endpoint_bind_helper;
    };

    class LIBATAPP_MACRO_API atapp_connector_impl {
    public:
#if defined(UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES) && UTIL_CONFIG_COMPILER_CXX_ALIAS_TEMPLATES
        using handle_set_t   = LIBATFRAME_UTILS_AUTO_SELETC_SET(atapp_connection_handle *);
        using protocol_set_t = LIBATFRAME_UTILS_AUTO_SELETC_SET(std::string);
#else
        typedef LIBATFRAME_UTILS_AUTO_SELETC_SET(atapp_connection_handle *) handle_set_t;
        typedef LIBATFRAME_UTILS_AUTO_SELETC_SET(std::string) protocol_set_t;
#endif

        struct address_type_t {
            enum type {
                EN_ACAT_NONE          = 0x0000,
                EN_ACAT_DUPLEX        = 0x0001,
                EN_ACAT_SIMPLEX       = 0x0002,
                EN_ACAT_LOCAL_HOST    = 0x0004,
                EN_ACAT_LOCAL_PROCESS = 0x0008,
            };
        };

        UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connector_impl)
        UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connector_impl)
    protected:
        LIBATAPP_MACRO_API atapp_connector_impl(app &owner);

        LIBATAPP_MACRO_API void register_protocol(const std::string &protocol_name);

        LIBATAPP_MACRO_API void cleanup();

    public:
        LIBATAPP_MACRO_API virtual ~atapp_connector_impl();
        LIBATAPP_MACRO_API virtual const char *name() UTIL_CONFIG_NOEXCEPT;

        /**
         * @brief get address type
         * @note address_type_t::EN_ACAT_SIMPLEX or address_type_t::EN_ACAT_DUPLEX must be set and only be set one of them
         *       address_type_t::EN_ACAT_LOCAL_HOST should be set if address can only be connected by local machine
         *       address_type_t::EN_ACAT_LOCAL_PROCESS should be set if address can only be connected by local process
         * @return must be xor of address_type_t::type
         */
        virtual uint32_t get_address_type(const atbus::channel::channel_address_t &addr) const = 0;

        /**
         * @brief callback for listen
         * @note just return non-zero when something wrong happend and this listen address will not be used
         * @return 0 or error code
         */
        LIBATAPP_MACRO_API virtual int32_t on_start_listen(const atbus::channel::channel_address_t &addr);

        /**
         * @brief callback for listen
         * @note need to store handle somewhere can call handle->set_ready() when the connection finished
         * @note just return non-zero when something wrong happend and this connection will not be used
         * @return 0 or error code
         */
        LIBATAPP_MACRO_API virtual int32_t on_start_connect(const etcd_discovery_node *discovery,
                                                            const atbus::channel::channel_address_t &addr,
                                                            const atapp_connection_handle::ptr_t &handle);
        LIBATAPP_MACRO_API virtual int32_t on_close_connect(atapp_connection_handle &handle); // can not renew handle any more
        LIBATAPP_MACRO_API virtual int32_t on_send_forward_request(atapp_connection_handle *handle, int32_t type, uint64_t *msg_sequence,
                                                                   const void *data, size_t data_size,
                                                                   const atapp::protocol::atapp_metadata *metadata);

        /**
         * @brief implement should call this when receive a response to tell app if a message is success delivered
         */
        LIBATAPP_MACRO_API virtual void on_receive_forward_response(atapp_connection_handle *handle, int32_t type, uint64_t msg_sequence,
                                                                    int32_t error_code, const void *data, size_t data_size,
                                                                    const atapp::protocol::atapp_metadata *metadata);

        LIBATAPP_MACRO_API virtual void on_discovery_event(etcd_discovery_action_t::type, const etcd_discovery_node::ptr_t &);

        LIBATAPP_MACRO_API const protocol_set_t &get_support_protocols() const UTIL_CONFIG_NOEXCEPT;

        UTIL_FORCEINLINE app *get_owner() const UTIL_CONFIG_NOEXCEPT { return owner_; }

    private:
        app *owner_;
        bool is_destroying_;
        handle_set_t handles_;
        protocol_set_t support_protocols_;
        mutable std::unique_ptr<util::scoped_demangled_name> auto_demangled_name_;

        friend struct atapp_connector_bind_helper;
    };

} // namespace atapp

#endif
