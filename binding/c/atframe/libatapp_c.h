#ifndef LIBATAPP_BINDING_C_LIBATAPP_C_H_
#define LIBATAPP_BINDING_C_LIBATAPP_C_H_

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atframe/atapp_config.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *libatapp_c_context;
typedef void *libatapp_c_custom_cmd_sender;

typedef const void *libatapp_c_message;

typedef void *libatapp_c_module;

enum LIBATAPP_C_ATBUS_PROTOCOL_CMD {
    LIBATAPP_C_ATBUS_CMD_INVALID = 0,

    //  数据协议
    LIBATAPP_C_ATBUS_CMD_CUSTOM_CMD_REQ     = 11,
    LIBATAPP_C_ATBUS_CMD_CUSTOM_CMD_RSP     = 12,
    LIBATAPP_C_ATBUS_CMD_DATA_TRANSFORM_REQ = 13,
    LIBATAPP_C_ATBUS_CMD_DATA_TRANSFORM_RSP = 14,

    // 节点控制协议
    LIBATAPP_C_ATBUS_CMD_NODE_SYNC_REQ = 15,
    LIBATAPP_C_ATBUS_CMD_NODE_SYNC_RSP = 16,
    LIBATAPP_C_ATBUS_CMD_NODE_REG_REQ  = 17,
    LIBATAPP_C_ATBUS_CMD_NODE_REG_RSP  = 18,
    LIBATAPP_C_ATBUS_CMD_NODE_CONN_SYN = 20,
    LIBATAPP_C_ATBUS_CMD_NODE_PING     = 21,
    LIBATAPP_C_ATBUS_CMD_NODE_PONG     = 22,

    LIBATAPP_C_ATBUS_CMD_MAX
};

// =========================== callbacks ===========================
typedef int32_t (*libatapp_c_on_msg_fn_t)(libatapp_c_context, libatapp_c_message, const void *msg_data, uint64_t msg_len, void *priv_data);
typedef int32_t (*libatapp_c_on_send_fail_fn_t)(libatapp_c_context, uint64_t src_pd, uint64_t dst_pd, libatapp_c_message, void *priv_data);
typedef int32_t (*libatapp_c_on_connected_fn_t)(libatapp_c_context, uint64_t pd, int32_t status, void *priv_data);
typedef int32_t (*libatapp_c_on_disconnected_fn_t)(libatapp_c_context, uint64_t pd, int32_t status, void *priv_data);
typedef int32_t (*libatapp_c_on_all_module_inited_fn_t)(libatapp_c_context, void *priv_data);
typedef int32_t (*libatapp_c_on_cmd_option_fn_t)(libatapp_c_context, libatapp_c_custom_cmd_sender, const char *buffer[], uint64_t buffer_len[], uint64_t sz,
                                                 void *priv_data);

LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_msg_fn(libatapp_c_context context, libatapp_c_on_msg_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_forward_response_fn(libatapp_c_context context, libatapp_c_on_send_fail_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_connected_fn(libatapp_c_context context, libatapp_c_on_connected_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_disconnected_fn(libatapp_c_context context, libatapp_c_on_disconnected_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_set_on_all_module_inited_fn(libatapp_c_context context, libatapp_c_on_all_module_inited_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_add_cmd(libatapp_c_context context, const char *cmd, libatapp_c_on_cmd_option_fn_t fn, const char *help_msg,
                                                   void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_add_option(libatapp_c_context context, const char *opt, libatapp_c_on_cmd_option_fn_t fn, const char *help_msg,
                                                      void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_custom_cmd_add_rsp(libatapp_c_custom_cmd_sender sender, const char *rsp, uint64_t rsp_sz);


// =========================== create/destory ===========================
LIBATAPP_MACRO_API libatapp_c_context __cdecl libatapp_c_create();
LIBATAPP_MACRO_API void __cdecl libatapp_c_destroy(libatapp_c_context context);

// =========================== actions ===========================
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_run(libatapp_c_context context, int32_t argc, const char **argv, void *priv_data);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_init(libatapp_c_context context, int32_t argc, const char **argv, void *priv_data);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_run_noblock(libatapp_c_context context, uint64_t max_event_count = 20000);

LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_reload(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_stop(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_tick(libatapp_c_context context);

// =========================== basic ===========================
LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_get_id(libatapp_c_context context);
LIBATAPP_MACRO_API void __cdecl libatapp_c_get_app_version(libatapp_c_context context, const char **verbuf, uint64_t *bufsz);

// =========================== configures ===========================
LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_get_configure_size(libatapp_c_context context, const char *path);
LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_get_configure(libatapp_c_context context, const char *path, const char *out_buf[], uint64_t out_len[],
                                                             uint64_t arr_sz);

// =========================== flags ===========================
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_inited(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_running(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_closing(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_closed(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_stoping(libatapp_c_context context);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_is_timeout(libatapp_c_context context);

// =========================== bus actions ===========================
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_listen(libatapp_c_context context, const char *address);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_connect(libatapp_c_context context, const char *address);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_disconnect(libatapp_c_context context, uint64_t app_id);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_send_data_msg(libatapp_c_context context, uint64_t app_id, int32_t type, const void *buffer, uint64_t sz,
                                                            int32_t require_rsp = 0);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_send_custom_msg(libatapp_c_context context, uint64_t app_id, const void *arr_buf[], uint64_t arr_size[],
                                                              uint64_t arr_count);

// =========================== message ===========================
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_msg_get_cmd(libatapp_c_message msg);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_msg_get_type(libatapp_c_message msg);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_msg_get_ret(libatapp_c_message msg);
LIBATAPP_MACRO_API uint32_t __cdecl libatapp_c_msg_get_sequence(libatapp_c_message msg);
LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_msg_get_src_bus_id(libatapp_c_message msg);
LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_msg_get_forward_from(libatapp_c_message msg);
LIBATAPP_MACRO_API uint64_t __cdecl libatapp_c_msg_get_forward_to(libatapp_c_message msg);

// =========================== module ===========================
LIBATAPP_MACRO_API libatapp_c_module __cdecl libatapp_c_module_create(libatapp_c_context context, const char *mod_name);
LIBATAPP_MACRO_API void __cdecl libatapp_c_module_get_name(libatapp_c_module mod, const char **namebuf, uint64_t *bufsz);
LIBATAPP_MACRO_API libatapp_c_context __cdecl libatapp_c_module_get_context(libatapp_c_module mod);
typedef int32_t (*libatapp_c_module_on_init_fn_t)(libatapp_c_module, void *priv_data);
typedef int32_t (*libatapp_c_module_on_reload_fn_t)(libatapp_c_module, void *priv_data);
typedef int32_t (*libatapp_c_module_on_stop_fn_t)(libatapp_c_module, void *priv_data);
typedef int32_t (*libatapp_c_module_on_timeout_fn_t)(libatapp_c_module, void *priv_data);
typedef void (*libatapp_c_module_on_cleanup_fn_t)(libatapp_c_module, void *priv_data);
typedef int32_t (*libatapp_c_module_on_tick_fn_t)(libatapp_c_module, void *priv_data);

LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_init(libatapp_c_module mod, libatapp_c_module_on_init_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_reload(libatapp_c_module mod, libatapp_c_module_on_reload_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_stop(libatapp_c_module mod, libatapp_c_module_on_stop_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_timeout(libatapp_c_module mod, libatapp_c_module_on_timeout_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_cleanup(libatapp_c_module mod, libatapp_c_module_on_cleanup_fn_t fn, void *priv_data);
LIBATAPP_MACRO_API void __cdecl libatapp_c_module_set_on_tick(libatapp_c_module mod, libatapp_c_module_on_tick_fn_t fn, void *priv_data);

// =========================== utilities ===========================
LIBATAPP_MACRO_API int64_t __cdecl libatapp_c_get_unix_timestamp();
/**
 * write log using atapp engine
 * @param tag tag, 0 for default
 * @param level log level
 * @param level_name log level name
 * @param file_path related file path
 * @param func_name related function name
 * @param line_number related line number
 * @param content log data
 */
LIBATAPP_MACRO_API void __cdecl libatapp_c_log_write(uint32_t tag, uint32_t level, const char *level_name, const char *file_path, const char *func_name,
                                                     uint32_t line_number, const char *content);
LIBATAPP_MACRO_API void __cdecl libatapp_c_log_update();

LIBATAPP_MACRO_API uint32_t __cdecl libatapp_c_log_get_level(uint32_t tag);
LIBATAPP_MACRO_API int32_t __cdecl libatapp_c_log_check_level(uint32_t tag, uint32_t level);
LIBATAPP_MACRO_API void __cdecl libatapp_c_log_set_project_directory(const char *project_dir, uint64_t dirsz);

#ifdef __cplusplus
}
#endif

#endif
