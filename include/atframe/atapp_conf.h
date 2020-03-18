/**
 * atapp_conf.h
 *
 *  Created on: 2016年04月23日
 *      Author: owent
 */
#ifndef LIBATAPP_ATAPP_CONF_H
#define LIBATAPP_ATAPP_CONF_H

#pragma once

#include "libatbus.h"
#include <string>
#include <vector>

#include "atapp_config.h"

namespace atapp {
    struct app_conf {
        // bus configure
        std::string id_cmd;
        atbus::node::bus_id_t id;
        std::vector<atbus::node::bus_id_t> id_mask; // convert a.b.c.d -> id
        std::string conf_file;
        std::string pid_file;
        const char *execute_path;
        bool resume_mode;
        bool remove_pidfile_after_exit;

        std::vector<std::string> bus_listen;
        atbus::node::conf_t bus_conf;
        std::string app_version;

        // app configure
        uint64_t stop_timeout;  // module timeout when receive a stop message, libuv use uint64_t
        uint64_t tick_interval; // tick interval, libuv use uint64_t

        atbus::node::bus_id_t type_id;
        std::string type_name;
        std::string name;
        std::string hash_code;
    };

    typedef enum {
        EN_ATAPP_ERR_SUCCESS = 0,
        EN_ATAPP_ERR_NOT_INITED = -1001,
        EN_ATAPP_ERR_ALREADY_INITED = -1002,
        EN_ATAPP_ERR_WRITE_PID_FILE = -1003,
        EN_ATAPP_ERR_SETUP_TIMER = -1004,
        EN_ATAPP_ERR_ALREADY_CLOSED = -1005,
        EN_ATAPP_ERR_MISSING_CONFIGURE_FILE = -1006,
        EN_ATAPP_ERR_LOAD_CONFIGURE_FILE = -1007,
        EN_ATAPP_ERR_SETUP_ATBUS = -1101,
        EN_ATAPP_ERR_SEND_FAILED = -1102,
        EN_ATAPP_ERR_COMMAND_IS_NULL = -1801,
        EN_ATAPP_ERR_NO_AVAILABLE_ADDRESS = -1802,
        EN_ATAPP_ERR_CONNECT_ATAPP_FAILED = -1803,
        EN_ATAPP_ERR_MIN = -1999,
    } ATAPP_ERROR_TYPE;
} // namespace atapp

#endif
