#ifndef ATFRAME_SERVICE_COMPONENT_MODULES_ETCD_MODULE_H
#define ATFRAME_SERVICE_COMPONENT_MODULES_ETCD_MODULE_H

#pragma once

#include <ctime>
#include <list>
#include <std/smart_ptr.h>
#include <string>
#include <vector>

#include <config/compiler/template_prefix.h>

#include <rapidjson/document.h>

#include <atframe/atapp_conf.h>

#include <config/compiler/template_suffix.h>

#include <network/http_request.h>
#include <random/random_generator.h>
#include <time/time_utility.h>


#include <atframe/atapp_module_impl.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_keepalive.h>
#include <atframe/etcdcli/etcd_watcher.h>

namespace atapp {
    class etcd_module : public ::atapp::module_impl {
    public:
        struct LIBATAPP_MACRO_API_HEAD_ONLY node_action_t {
            enum type {
                EN_NAT_UNKNOWN = 0,
                EN_NAT_PUT,
                EN_NAT_DELETE,
            };
        };
        struct LIBATAPP_MACRO_API_HEAD_ONLY node_info_t {
            atapp::protocol::atapp_discovery node_discovery;
            node_action_t::type action;
        };

        struct LIBATAPP_MACRO_API_HEAD_ONLY node_list_t {
            std::list<node_info_t> nodes;
        };

        struct LIBATAPP_MACRO_API_HEAD_ONLY watcher_sender_list_t {
            std::reference_wrapper<etcd_module> atapp_module;
            std::reference_wrapper<const ::atapp::etcd_response_header> etcd_header;
            std::reference_wrapper<const ::atapp::etcd_watcher::response_t> etcd_body;
            std::reference_wrapper<const ::atapp::etcd_watcher::event_t> event;
            std::reference_wrapper<const node_info_t> node;

            inline watcher_sender_list_t(etcd_module &m, const ::atapp::etcd_response_header &h, const ::atapp::etcd_watcher::response_t &b,
                                         const ::atapp::etcd_watcher::event_t &e, const node_info_t &n)
                : atapp_module(std::ref(m)), etcd_header(std::cref(h)), etcd_body(std::cref(b)), event(std::cref(e)), node(std::cref(n)) {}
        };

        struct LIBATAPP_MACRO_API_HEAD_ONLY watcher_sender_one_t {
            std::reference_wrapper<etcd_module> atapp_module;
            std::reference_wrapper<const ::atapp::etcd_response_header> etcd_header;
            std::reference_wrapper<const ::atapp::etcd_watcher::response_t> etcd_body;
            std::reference_wrapper<const ::atapp::etcd_watcher::event_t> event;
            std::reference_wrapper<node_info_t> node;

            inline watcher_sender_one_t(etcd_module &m, const ::atapp::etcd_response_header &h, const ::atapp::etcd_watcher::response_t &b,
                                        const ::atapp::etcd_watcher::event_t &e, node_info_t &n)
                : atapp_module(std::ref(m)), etcd_header(std::cref(h)), etcd_body(std::cref(b)), event(std::cref(e)), node(std::ref(n)) {}
        };

        typedef std::function<void(watcher_sender_list_t &)> watcher_list_callback_t;
        typedef std::function<void(watcher_sender_one_t &)> watcher_one_callback_t;

    public:
        LIBATAPP_MACRO_API etcd_module();
        LIBATAPP_MACRO_API virtual ~etcd_module();

    public:
        LIBATAPP_MACRO_API void reset();

        LIBATAPP_MACRO_API virtual int init() UTIL_CONFIG_OVERRIDE;

        LIBATAPP_MACRO_API virtual int reload() UTIL_CONFIG_OVERRIDE;

        LIBATAPP_MACRO_API virtual int stop() UTIL_CONFIG_OVERRIDE;

        LIBATAPP_MACRO_API virtual int timeout() UTIL_CONFIG_OVERRIDE;

        LIBATAPP_MACRO_API virtual const char *name() const UTIL_CONFIG_OVERRIDE;

        LIBATAPP_MACRO_API virtual int tick() UTIL_CONFIG_OVERRIDE;

        LIBATAPP_MACRO_API const std::string &get_conf_custom_data() const;
        LIBATAPP_MACRO_API void set_conf_custom_data(const std::string &v);

        LIBATAPP_MACRO_API std::string get_by_id_path() const;
        LIBATAPP_MACRO_API std::string get_by_type_id_path() const;
        LIBATAPP_MACRO_API std::string get_by_type_name_path() const;
        LIBATAPP_MACRO_API std::string get_by_name_path() const;
        LIBATAPP_MACRO_API std::string get_by_tag_path(const std::string &tag_name) const;

        LIBATAPP_MACRO_API std::string get_by_id_watcher_path() const;
        LIBATAPP_MACRO_API std::string get_by_type_id_watcher_path(uint64_t type_id) const;
        LIBATAPP_MACRO_API std::string get_by_type_name_watcher_path(const std::string &type_name) const;
        LIBATAPP_MACRO_API std::string get_by_name_watcher_path() const;
        LIBATAPP_MACRO_API std::string get_by_tag_watcher_path(const std::string &tag_name) const;

        LIBATAPP_MACRO_API int add_watcher_by_id(watcher_list_callback_t fn);
        LIBATAPP_MACRO_API int add_watcher_by_type_id(uint64_t type_id, watcher_one_callback_t fn);
        LIBATAPP_MACRO_API int add_watcher_by_type_name(const std::string &type_name, watcher_one_callback_t fn);
        LIBATAPP_MACRO_API int add_watcher_by_name(watcher_list_callback_t fn);
        LIBATAPP_MACRO_API int add_watcher_by_tag(const std::string &tag_name, watcher_one_callback_t fn);

        LIBATAPP_MACRO_API atapp::etcd_watcher::ptr_t add_watcher_by_custom_path(const std::string &custom_path, watcher_one_callback_t fn);

        LIBATAPP_MACRO_API const ::atapp::etcd_cluster &get_raw_etcd_ctx() const;
        LIBATAPP_MACRO_API ::atapp::etcd_cluster &get_raw_etcd_ctx();

        LIBATAPP_MACRO_API const atapp::protocol::atapp_etcd &get_configure() const;
        LIBATAPP_MACRO_API const std::string &get_configure_path() const;

        LIBATAPP_MACRO_API atapp::etcd_keepalive::ptr_t add_keepalive_actor(std::string &val, const std::string &node_path);

    private:
        static bool unpack(node_info_t &out, const std::string &path, const std::string &json, bool reset_data);
        static void pack(const node_info_t &out, std::string &json);

        static int http_callback_on_etcd_closed(util::network::http_request &req);

        struct watcher_callback_list_wrapper_t {
            etcd_module *mod;
            std::list<watcher_list_callback_t> *callbacks;

            watcher_callback_list_wrapper_t(etcd_module &m, std::list<watcher_list_callback_t> &cbks);
            void operator()(const ::atapp::etcd_response_header &header, const ::atapp::etcd_watcher::response_t &evt_data);
        };

        struct watcher_callback_one_wrapper_t {
            etcd_module *mod;
            watcher_one_callback_t callback;

            watcher_callback_one_wrapper_t(etcd_module &m, watcher_one_callback_t cbk);
            void operator()(const ::atapp::etcd_response_header &header, const ::atapp::etcd_watcher::response_t &evt_data);
        };

    private:
        std::string conf_path_cache_;
        std::string custom_data_;
        util::network::http_request::curl_m_bind_ptr_t curl_multi_;
        util::network::http_request::ptr_t cleanup_request_;
        ::atapp::etcd_cluster etcd_ctx_;
        std::list<watcher_list_callback_t> watcher_by_id_callbacks_;
        std::list<watcher_list_callback_t> watcher_by_name_callbacks_;
    };
} // namespace atapp

#endif