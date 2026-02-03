// Copyright 2026 atframework
//
// Created by owent on

#pragma once

#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

#include <config/compiler/protobuf_prefix.h>

#include <rapidjson/document.h>

#include <config/compiler/protobuf_suffix.h>

#include <gsl/select-gsl.h>

#include <log/log_wrapper.h>

#include <libatbus.h>

#include "atframe/atapp_config.h"

namespace google {
namespace protobuf {
class Message;
class Timestamp;
class Duration;
}  // namespace protobuf
}  // namespace google

LIBATAPP_MACRO_NAMESPACE_BEGIN

struct LIBATAPP_MACRO_API_HEAD_ONLY rapidjson_loader_string_mode {
  enum type {
    RAW = 0,
    URI,
    URI_COMPONENT,
  };
};

struct LIBATAPP_MACRO_API_HEAD_ONLY rapidjson_loader_load_options {
  bool reserve_empty;
  bool convert_large_number_to_string;  // it's friendly to JSON.parse(...) in javascript
  rapidjson_loader_string_mode::type string_mode;

  inline rapidjson_loader_load_options()
      : reserve_empty(false), convert_large_number_to_string(false), string_mode(rapidjson_loader_string_mode::RAW) {}
};

struct LIBATAPP_MACRO_API_HEAD_ONLY rapidjson_loader_dump_options {
  rapidjson_loader_string_mode::type string_mode;
  bool convert_number_from_string;  // it's friendly to JSON.parse(...) in javascript
  inline rapidjson_loader_dump_options()
      : string_mode(rapidjson_loader_string_mode::RAW), convert_number_from_string(true) {}
};

LIBATAPP_MACRO_API gsl::span<unsigned char> rapidjson_loader_get_default_shared_buffer();

LIBATAPP_MACRO_API std::string rapidjson_loader_stringify(const rapidjson::Document &doc, size_t more_reserve_size = 0);
LIBATAPP_MACRO_API bool rapidjson_loader_unstringify(rapidjson::Document &doc, const std::string &json);
LIBATAPP_MACRO_API const char *rapidjson_loader_get_type_name(rapidjson::Type t);

LIBATAPP_MACRO_API std::string rapidjson_loader_stringify(
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
    const rapidjson_loader_load_options &options = rapidjson_loader_load_options(),
    gsl::span<unsigned char> use_buffer = {});
LIBATAPP_MACRO_API bool rapidjson_loader_parse(
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst, const std::string &src,
    const rapidjson_loader_dump_options &options = rapidjson_loader_dump_options(),
    gsl::span<unsigned char> use_buffer = {});

LIBATAPP_MACRO_API void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                            rapidjson::Value &&val, rapidjson::Document &doc);
LIBATAPP_MACRO_API void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                            const rapidjson::Value &val, rapidjson::Document &doc);
LIBATAPP_MACRO_API void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                            gsl::string_view val, rapidjson::Document &doc);

LIBATAPP_MACRO_API void rapidjson_loader_append_to_list(rapidjson::Value &list_parent, gsl::string_view val,
                                                        rapidjson::Document &doc);

LIBATAPP_MACRO_API void rapidjson_loader_dump_to(
    const rapidjson::Document &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
    const rapidjson_loader_dump_options &options = rapidjson_loader_dump_options());

LIBATAPP_MACRO_API void rapidjson_loader_load_from(
    rapidjson::Document &dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
    const rapidjson_loader_load_options &options = rapidjson_loader_load_options());

LIBATAPP_MACRO_API void rapidjson_loader_dump_to(
    const rapidjson::Value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
    const rapidjson_loader_dump_options &options = rapidjson_loader_dump_options());

LIBATAPP_MACRO_API void rapidjson_loader_load_from(
    rapidjson::Value &dst, rapidjson::Document &doc, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
    const rapidjson_loader_load_options &options = rapidjson_loader_load_options());

// ============ template implement ============

template <class TVAL, bool>
struct rapidjson_loader_mutable_member_helper;

template <class TVAL>
struct rapidjson_loader_mutable_member_helper<TVAL, true> {
  static LIBATAPP_MACRO_API_HEAD_ONLY inline void set(rapidjson::Value &parent, gsl::string_view key, TVAL &&val,
                                                      rapidjson::Document &doc) {
    rapidjson_loader_mutable_set_member(parent, key, gsl::string_view(std::forward<TVAL>(val)), doc);
  }

  static LIBATAPP_MACRO_API_HEAD_ONLY inline void append(rapidjson::Value &parent, TVAL &&val,
                                                         rapidjson::Document &doc) {
    rapidjson_loader_append_to_list(parent, gsl::string_view(std::forward<TVAL>(val)), doc);
  }
};

template <class TVAL>
struct rapidjson_loader_mutable_member_helper<TVAL, false> {
  static LIBATAPP_MACRO_API_HEAD_ONLY inline void set(rapidjson::Value &parent, gsl::string_view key, TVAL &&val,
                                                      rapidjson::Document &doc) {
    if (parent.IsNull()) {
      parent.SetObject();
    }

    if (!parent.IsObject()) {
      FWLOGERROR("parent should be a object, but we got {}.", rapidjson_loader_get_type_name(parent.GetType()));
      return;
    }

    rapidjson::Value jkey;
    jkey.SetString(rapidjson::StringRef(key.data(), static_cast<rapidjson::SizeType>(key.size())));
    rapidjson::Value::MemberIterator iter = parent.FindMember(jkey);
    if (iter != parent.MemberEnd()) {
      iter->value.Set(std::forward<TVAL>(val), doc.GetAllocator());
    } else {
      rapidjson::Value k;
      k.SetString(key.data(), static_cast<rapidjson::SizeType>(key.size()), doc.GetAllocator());
      parent.AddMember(k, std::forward<TVAL>(val), doc.GetAllocator());
    }
  }

  static LIBATAPP_MACRO_API_HEAD_ONLY inline void append(rapidjson::Value &parent, TVAL &&val,
                                                         rapidjson::Document &doc) {
    if (parent.IsNull()) {
      parent.SetArray();
    }

    if (!parent.IsArray()) {
      FWLOGERROR("parent should be a array, but we got {}.", rapidjson_loader_get_type_name(parent.GetType()));
      return;
    }

    parent.PushBack(std::forward<TVAL>(val), doc.GetAllocator());
  }
};

template <class TVAL, class = typename std::enable_if<
                          !std::is_same<typename std::decay<TVAL>::type, gsl::string_view>::value>::type>
LIBATAPP_MACRO_API_HEAD_ONLY void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                                      TVAL &&val, rapidjson::Document &doc) {
  rapidjson_loader_mutable_member_helper<TVAL, std::is_convertible<TVAL, gsl::string_view>::value>::set(
      parent, key, std::forward<TVAL>(val), doc);
}

template <class TVAL, class = typename std::enable_if<
                          !std::is_same<typename std::decay<TVAL>::type, gsl::string_view>::value>::type>
LIBATAPP_MACRO_API_HEAD_ONLY void rapidjson_loader_append_to_list(rapidjson::Value &parent, TVAL &&val,
                                                                  rapidjson::Document &doc) {
  rapidjson_loader_mutable_member_helper<TVAL, std::is_convertible<TVAL, gsl::string_view>::value>::append(
      parent, std::forward<TVAL>(val), doc);
}
LIBATAPP_MACRO_NAMESPACE_END

