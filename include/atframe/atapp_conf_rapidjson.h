#ifndef LIBATAPP_ATAPP_CONF_RAPIDJSON_H
#define LIBATAPP_ATAPP_CONF_RAPIDJSON_H

#pragma once
#include <cstddef>
#include <cstring>
#include <memory>
#include <stdint.h>
#include <string>
#include <type_traits>

#include "libatbus.h"

#include <config/compiler/protobuf_prefix.h>

#include <rapidjson/document.h>

#include <config/compiler/protobuf_suffix.h>

#include <log/log_wrapper.h>

#include "atapp_config.h"

namespace google {
    namespace protobuf {
        class Message;
        class Timestamp;
        class Duration;
    } // namespace protobuf
} // namespace google

namespace atapp {

    struct LIBATAPP_MACRO_API_HEAD_ONLY rapidsjon_loader_string_mode {
        enum type {
            RAW = 0,
            URI,
            URI_COMPONENT,
        };
    };

    struct LIBATAPP_MACRO_API_HEAD_ONLY rapidsjon_loader_load_options {
        bool reserve_empty;
        bool convert_large_number_to_string; // it's friendly to JSON.parse(...) in javascript
        rapidsjon_loader_string_mode::type string_mode;

        inline rapidsjon_loader_load_options()
            : reserve_empty(false), convert_large_number_to_string(false), string_mode(rapidsjon_loader_string_mode::RAW) {}
    };

    struct LIBATAPP_MACRO_API_HEAD_ONLY rapidsjon_loader_dump_options {
        rapidsjon_loader_string_mode::type string_mode;
        bool convert_number_from_string; // it's friendly to JSON.parse(...) in javascript
        inline rapidsjon_loader_dump_options() : string_mode(rapidsjon_loader_string_mode::RAW), convert_number_from_string(true) {}
    };

    LIBATAPP_MACRO_API std::string rapidsjon_loader_stringify(const rapidjson::Document &doc, size_t more_reserve_size = 0);
    LIBATAPP_MACRO_API bool rapidsjon_loader_unstringify(rapidjson::Document &doc, const std::string &json);
    LIBATAPP_MACRO_API const char *rapidsjon_loader_get_type_name(rapidjson::Type t);

    LIBATAPP_MACRO_API std::string
    rapidsjon_loader_stringify(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                               const rapidsjon_loader_load_options &options = rapidsjon_loader_load_options());
    LIBATAPP_MACRO_API bool rapidsjon_loader_parse(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst, const std::string &src,
                                                   const rapidsjon_loader_dump_options &options = rapidsjon_loader_dump_options());

    template <typename TVAL>
    LIBATAPP_MACRO_API_HEAD_ONLY void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, TVAL &&val,
                                                                          rapidjson::Document &doc);

    LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, rapidjson::Value &&val,
                                                                rapidjson::Document &doc);
    LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, const rapidjson::Value &val,
                                                                rapidjson::Document &doc);
    LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, const std::string &val,
                                                                rapidjson::Document &doc);
    LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, std::string &val,
                                                                rapidjson::Document &doc);
    LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, const char *val,
                                                                rapidjson::Document &doc);

    template <typename TVAL>
    LIBATAPP_MACRO_API_HEAD_ONLY void rapidsjon_loader_append_to_list(rapidjson::Value &list_parent, TVAL &&val, rapidjson::Document &doc);

    LIBATAPP_MACRO_API void rapidsjon_loader_append_to_list(rapidjson::Value &list_parent, const std::string &val,
                                                            rapidjson::Document &doc);
    LIBATAPP_MACRO_API void rapidsjon_loader_append_to_list(rapidjson::Value &list_parent, std::string &val, rapidjson::Document &doc);
    LIBATAPP_MACRO_API void rapidsjon_loader_append_to_list(rapidjson::Value &list_parent, const char *val, rapidjson::Document &doc);

    LIBATAPP_MACRO_API void rapidsjon_loader_dump_to(const rapidjson::Document &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                     const rapidsjon_loader_dump_options &options = rapidsjon_loader_dump_options());

    LIBATAPP_MACRO_API void rapidsjon_loader_load_from(rapidjson::Document &dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                                                       const rapidsjon_loader_load_options &options = rapidsjon_loader_load_options());

    LIBATAPP_MACRO_API void rapidsjon_loader_dump_to(const rapidjson::Value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                     const rapidsjon_loader_dump_options &options = rapidsjon_loader_dump_options());

    LIBATAPP_MACRO_API void rapidsjon_loader_load_from(rapidjson::Value &dst, rapidjson::Document &doc,
                                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                                                       const rapidsjon_loader_load_options &options = rapidsjon_loader_load_options());

    // ============ template implement ============

    template <typename TVAL>
    LIBATAPP_MACRO_API_HEAD_ONLY void rapidsjon_loader_mutable_set_member(rapidjson::Value &parent, const char *key, TVAL &&val,
                                                                          rapidjson::Document &doc) {
        if (!parent.IsObject()) {
            parent.SetObject();
        }

        rapidjson::Value::MemberIterator iter = parent.FindMember(key);
        if (iter != parent.MemberEnd()) {
            iter->value.Set(std::forward<TVAL>(val), doc.GetAllocator());
        } else {
            rapidjson::Value k;
            k.SetString(key, doc.GetAllocator());
            parent.AddMember(k, std::forward<TVAL>(val), doc.GetAllocator());
        }
    }

    template <typename TVAL>
    LIBATAPP_MACRO_API_HEAD_ONLY void rapidsjon_loader_append_to_list(rapidjson::Value &list_parent, TVAL &&val, rapidjson::Document &doc) {
        if (list_parent.IsArray()) {
            list_parent.PushBack(std::forward<TVAL>(val), doc.GetAllocator());
        } else {
            WLOGERROR("parent should be a array, but we got %s.", rapidsjon_loader_get_type_name(list_parent.GetType()));
        }
    }
} // namespace atapp

#endif
