// Copyright 2026 atframework
//
// Created by owent

#pragma once

#include <config/compile_optimize.h>
#include <config/compiler_features.h>

#include <design_pattern/nomovable.h>
#include <design_pattern/noncopyable.h>

#include <common/demangle.h>

#include <detail/buffer.h>
#include <detail/libatbus_channel_export.h>

#include <memory>
#include <string>
#include <unordered_set>

#include "atframe/atapp_config.h"
#include "atframe/etcdcli/etcd_discovery.h"

LIBATAPP_MACRO_NAMESPACE_BEGIN

class app;
class atapp_endpoint;
class atapp_connector_impl;
class atapp_connection_handle;

struct atapp_connector_bind_helper {
  // This API is used by inner system and will not be exported, do not call it directly
  static LIBATAPP_MACRO_API_SYMBOL_HIDDEN void unbind(atapp_connection_handle &handle, atapp_connector_impl &connect);
  // This API is used by inner system and will not be exported, do not call it directly
  static LIBATAPP_MACRO_API_SYMBOL_HIDDEN void bind(atapp_connection_handle &handle, atapp_connector_impl &connect);
};

class atapp_connection_handle {
 public:
  using ptr_t = std::shared_ptr<atapp_connection_handle>;
  using weak_ptr_t = std::weak_ptr<atapp_connection_handle>;
  using on_destroy_fn_type = std::function<void(atapp_connection_handle &)>;

  enum class flags_t : uint32_t {
    kNone = 0x00,
    kClosing = 0x01,
    kReady = 0x02,
  };

  UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connection_handle)
  UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connection_handle)

 public:
  LIBATAPP_MACRO_API atapp_connection_handle();
  LIBATAPP_MACRO_API ~atapp_connection_handle();
  LIBATAPP_MACRO_API void close();
  LIBATAPP_MACRO_API bool is_closing() const noexcept;
  LIBATAPP_MACRO_API void set_ready() noexcept;
  LIBATAPP_MACRO_API void set_unready() noexcept;
  LIBATAPP_MACRO_API bool is_ready() const noexcept;

  ATFW_UTIL_FORCEINLINE void set_private_data_ptr(void *input) noexcept { private_data_ptr_ = input; }
  ATFW_UTIL_FORCEINLINE void *get_private_data_ptr() const noexcept { return private_data_ptr_; }
  ATFW_UTIL_FORCEINLINE void set_private_data_u64(uint64_t input) noexcept { private_data_u64_ = input; }
  ATFW_UTIL_FORCEINLINE uint64_t get_private_data_u64() const noexcept { return private_data_u64_; }
  ATFW_UTIL_FORCEINLINE void set_private_data_i64(int64_t input) noexcept { private_data_i64_ = input; }
  ATFW_UTIL_FORCEINLINE int64_t get_private_data_i64() const noexcept { return private_data_i64_; }
  ATFW_UTIL_FORCEINLINE void set_private_data_uptr(uintptr_t input) noexcept { private_data_uptr_ = input; }
  ATFW_UTIL_FORCEINLINE uintptr_t get_private_data_uptr() const noexcept { return private_data_uptr_; }
  ATFW_UTIL_FORCEINLINE void set_private_data_iptr(intptr_t input) noexcept { private_data_iptr_ = input; }
  ATFW_UTIL_FORCEINLINE intptr_t get_private_data_iptr() const noexcept { return private_data_iptr_; }

  LIBATAPP_MACRO_API void set_on_destroy(on_destroy_fn_type fn);
  LIBATAPP_MACRO_API const on_destroy_fn_type &get_on_destroy() const;
  LIBATAPP_MACRO_API void reset_on_destroy();

  ATFW_UTIL_FORCEINLINE atapp_connector_impl *get_connector() const noexcept { return connector_; }
  ATFW_UTIL_FORCEINLINE atapp_endpoint *get_endpoint() const noexcept { return endpiont_; }

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
  on_destroy_fn_type on_destroy_fn_;

  friend struct atapp_connector_bind_helper;
  friend struct atapp_endpoint_bind_helper;
};

class LIBATAPP_MACRO_API_SYMBOL_VISIBLE atapp_connector_impl {
 public:
  using handle_set_t = std::unordered_set<atapp_connection_handle *>;
  using protocol_set_t = std::unordered_set<std::string>;

  enum class address_type_t : uint32_t {
    kNone = 0x0000,
    kDuplex = 0x0001,
    kSimplex = 0x0002,
    kLocalHost = 0x0004,
    kLocalProcess = 0x0008,
  };

  UTIL_DESIGN_PATTERN_NOCOPYABLE(atapp_connector_impl)
  UTIL_DESIGN_PATTERN_NOMOVABLE(atapp_connector_impl)

 protected:
  LIBATAPP_MACRO_API atapp_connector_impl(app &owner);

  LIBATAPP_MACRO_API void register_protocol(const std::string &protocol_name);

  LIBATAPP_MACRO_API void cleanup();

 public:
  LIBATAPP_MACRO_API virtual ~atapp_connector_impl();
  LIBATAPP_MACRO_API virtual const char *name() noexcept;

  /**
   * @brief get address type
   * @note address_type_t::kSimplex or address_type_t::kDuplex must be set and only be set one of them
   *       address_type_t::kLocalHost should be set if address can only be connected by local machine
   *       address_type_t::kLocalProcess should be set if address can only be connected by local process
   * @return must be xor of address_type_t
   */
  virtual uint32_t get_address_type(const atbus::channel::channel_address_t &addr) const noexcept = 0;

  /**
   * @brief if this connector is allowed to connect to loopback endpoint
   * @note false for default
   * @return true if this connector
   */
  LIBATAPP_MACRO_API bool support_loopback() const noexcept;

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
  LIBATAPP_MACRO_API virtual int32_t on_start_connect(const etcd_discovery_node &discovery, atapp_endpoint &endpoint,
                                                      const atbus::channel::channel_address_t &addr,
                                                      const atapp_connection_handle::ptr_t &handle);
  LIBATAPP_MACRO_API virtual int32_t on_close_connection(
      atapp_connection_handle &handle);  // can not renew handle any more
  LIBATAPP_MACRO_API virtual int32_t on_send_forward_request(atapp_connection_handle *handle, int32_t type,
                                                             uint64_t *msg_sequence,
                                                             gsl::span<const unsigned char> data,
                                                             const atapp::protocol::atapp_metadata *metadata);

  /**
   * @brief implement should call this when receive a response to tell app if a message is success delivered
   */
  LIBATAPP_MACRO_API virtual void on_receive_forward_response(atapp_connection_handle *handle, int32_t type,
                                                              uint64_t msg_sequence, int32_t error_code,
                                                              gsl::span<const unsigned char> data,
                                                              const atapp::protocol::atapp_metadata *metadata);

  LIBATAPP_MACRO_API virtual void on_discovery_event(etcd_discovery_action_t, const etcd_discovery_node::ptr_t &);

  LIBATAPP_MACRO_API const protocol_set_t &get_support_protocols() const noexcept;

  ATFW_UTIL_FORCEINLINE app *ATFW_UTIL_MACRO_NONNULL get_owner() const noexcept { return owner_; }

 private:
  app *ATFW_UTIL_MACRO_NONNULL owner_;
  bool is_destroying_;
  handle_set_t handles_;
  protocol_set_t support_protocols_;
  mutable std::unique_ptr<atfw::util::scoped_demangled_name> auto_demangled_name_;

  friend struct atapp_connector_bind_helper;
  friend class app;
};

LIBATAPP_MACRO_NAMESPACE_END
