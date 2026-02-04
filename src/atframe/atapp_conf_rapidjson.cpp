// Copyright 2026 atframework
//
// Created by owent

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

// clang-format off
#include <config/compiler/protobuf_prefix.h>
// clang-format on

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/repeated_field.h>

// clang-format off
#include <config/compiler/protobuf_suffix.h>
// clang-format on

#include "atframe/atapp_conf_rapidjson.h"

#ifdef GetMessage
#  undef GetMessage
#endif

#if defined(max)
#  undef max
#endif

#include <config/atframe_utils_build_feature.h>

#include <common/string_oprs.h>
#include <std/thread.h>
#include <string/tquerystring.h>

#include <assert.h>

#if defined(ATFRAMEWORK_UTILS_THREAD_TLS_USE_PTHREAD) && ATFRAMEWORK_UTILS_THREAD_TLS_USE_PTHREAD
#  include <pthread.h>
#endif

#include <memory>
#include <numeric>
#include <vector>

LIBATAPP_MACRO_NAMESPACE_BEGIN
namespace detail {
#if defined(ATFRAMEWORK_UTILS_THREAD_TLS_USE_PTHREAD) && ATFRAMEWORK_UTILS_THREAD_TLS_USE_PTHREAD
static pthread_once_t gt_rapidjson_loader_get_shared_buffer_tls_once = PTHREAD_ONCE_INIT;
static pthread_key_t gt_rapidjson_loader_get_shared_buffer_tls_key;

static void dtor_pthread_rapidjson_loader_get_shared_buffer_tls(void *p) {
  unsigned char *res = reinterpret_cast<unsigned char *>(p);
  if (nullptr != res) {
    delete[] res;
  }
}

static void init_pthread_rapidjson_loader_get_shared_buffer_tls() {
  (void)pthread_key_create(&gt_rapidjson_loader_get_shared_buffer_tls_key,
                           dtor_pthread_rapidjson_loader_get_shared_buffer_tls);
}

static gsl::span<unsigned char> rapidjson_loader_get_shared_buffer() {
  (void)pthread_once(&gt_rapidjson_loader_get_shared_buffer_tls_once,
                     init_pthread_rapidjson_loader_get_shared_buffer_tls);
  unsigned char *ret =
      reinterpret_cast<unsigned char *>(pthread_getspecific(gt_rapidjson_loader_get_shared_buffer_tls_key));
  if (nullptr == ret) {
    ret = new unsigned char[2 * 1024 * 1024];  // in case of padding
    pthread_setspecific(gt_rapidjson_loader_get_shared_buffer_tls_key, ret);
  }
  return {ret, 2 * 1024 * 1024};
}
#else
static gsl::span<unsigned char> rapidjson_loader_get_shared_buffer() {
  static THREAD_TLS unsigned char ret[2 * 1024 * 1024];  // in case of padding
  return {ret, 2 * 1024 * 1024};
}
#endif

static void load_field_string_filter(const std::string &input, rapidjson::Value &output, rapidjson::Document &doc,
                                     const rapidjson_loader_load_options &options) {
  switch (options.string_mode) {
    case rapidjson_loader_string_mode::kUri: {
      std::string strv = atfw::util::uri::encode_uri(input.c_str(), input.size());
      output.SetString(strv.c_str(), static_cast<rapidjson::SizeType>(strv.size()), doc.GetAllocator());
      break;
    }
    case rapidjson_loader_string_mode::kUriComponent: {
      std::string strv = atfw::util::uri::encode_uri_component(input.c_str(), input.size());
      output.SetString(strv.c_str(), static_cast<rapidjson::SizeType>(strv.size()), doc.GetAllocator());
      break;
    }
    default: {
      output.SetString(input.c_str(), static_cast<rapidjson::SizeType>(input.size()), doc.GetAllocator());
      break;
    }
  }
}

static void load_map_field_item(rapidjson::Value &dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                                const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds, rapidjson::Document &doc,
                                const rapidjson_loader_load_options &options) {
  if (nullptr == fds) {
    return;
  }
  if (fds->is_repeated()) {
    FWLOGERROR("{} in {} with type={} can not be repeated", fds->name(), fds->containing_type()->full_name(),
               fds->type_name());
    return;
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      dst.Set(src.GetReflection()->GetInt32(src, fds), doc.GetAllocator());
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      int64_t int_val = src.GetReflection()->GetInt64(src, fds);
      if (options.convert_large_number_to_string && int_val > std::numeric_limits<int32_t>::max()) {
        char str_val[24] = {0};
        atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
        dst.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
      } else {
        dst.Set(int_val, doc.GetAllocator());
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t int_val = src.GetReflection()->GetUInt32(src, fds);
      if (options.convert_large_number_to_string &&
          int_val > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        char str_val[24] = {0};
        atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
        dst.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
      } else {
        dst.Set(int_val, doc.GetAllocator());
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t int_val = src.GetReflection()->GetUInt64(src, fds);
      if (options.convert_large_number_to_string &&
          int_val > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
        char str_val[24] = {0};
        atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
        dst.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
      } else {
        dst.Set(int_val, doc.GetAllocator());
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      std::string empty;
      load_field_string_filter(src.GetReflection()->GetStringReference(src, fds, &empty), dst, doc, options);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      dst.SetObject();
      if (dst.IsObject()) {
        rapidjson_loader_load_from(dst, doc, src.GetReflection()->GetMessage(src, fds), options);
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      dst.Set(src.GetReflection()->GetDouble(src, fds), doc.GetAllocator());
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      dst.Set(src.GetReflection()->GetFloat(src, fds), doc.GetAllocator());
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      dst.Set(src.GetReflection()->GetBool(src, fds), doc.GetAllocator());
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      dst.Set(src.GetReflection()->GetEnumValue(src, fds), doc.GetAllocator());
      break;
    };
    default: {
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), fds->containing_type()->full_name(),
                 fds->type_name());
      break;
    }
  }
}

static rapidjson::Value load_field_item_to_map_string(const ::google::protobuf::Message &src,
                                                      const ::google::protobuf::FieldDescriptor *fds,
                                                      rapidjson::Document &doc) {
  rapidjson::Value ret;

  if (nullptr == fds) {
    ret.SetString("", 0, doc.GetAllocator());
    return ret;
  }

  // map key can not be repeated in protobuf
  if (fds->is_repeated()) {
    ret.SetString("", 0, doc.GetAllocator());
    return ret;
  }

  char integer_string[24] = {0};
  switch (fds->cpp_type()) {
    case google::protobuf::FieldDescriptor::CPPTYPE_INT32: {
      size_t str_len = atfw::util::string::int2str(integer_string, sizeof(integer_string) - 1,
                                                   src.GetReflection()->GetInt32(src, fds));
      ret.SetString(integer_string, static_cast<rapidjson::SizeType>(str_len), doc.GetAllocator());
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT32: {
      size_t str_len = atfw::util::string::int2str(integer_string, sizeof(integer_string) - 1,
                                                   src.GetReflection()->GetUInt32(src, fds));
      ret.SetString(integer_string, static_cast<rapidjson::SizeType>(str_len), doc.GetAllocator());
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_INT64: {
      size_t str_len = atfw::util::string::int2str(integer_string, sizeof(integer_string) - 1,
                                                   src.GetReflection()->GetInt64(src, fds));
      ret.SetString(integer_string, static_cast<rapidjson::SizeType>(str_len), doc.GetAllocator());
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_UINT64: {
      size_t str_len = atfw::util::string::int2str(integer_string, sizeof(integer_string) - 1,
                                                   src.GetReflection()->GetUInt64(src, fds));
      ret.SetString(integer_string, static_cast<rapidjson::SizeType>(str_len), doc.GetAllocator());
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_FLOAT: {
      std::string float_string = atfw::util::log::format("{}", src.GetReflection()->GetFloat(src, fds));
      ret.SetString(float_string.c_str(), static_cast<rapidjson::SizeType>(float_string.size()), doc.GetAllocator());
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE: {
      std::string double_string = atfw::util::log::format("{}", src.GetReflection()->GetDouble(src, fds));
      ret.SetString(double_string.c_str(), static_cast<rapidjson::SizeType>(double_string.size()), doc.GetAllocator());
      break;
    }
    case google::protobuf::FieldDescriptor::CPPTYPE_ENUM: {
      size_t str_len = atfw::util::string::int2str(integer_string, sizeof(integer_string) - 1,
                                                   src.GetReflection()->GetEnumValue(src, fds));
      ret.SetString(integer_string, static_cast<rapidjson::SizeType>(str_len), doc.GetAllocator());
      break;
    }
    default: {
      ret.SetString("", 0, doc.GetAllocator());
      break;
    }
  }

  return ret;
}

static void load_field_item(rapidjson::Value &parent, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds, rapidjson::Document &doc,
                            const rapidjson_loader_load_options &options) {
  if (nullptr == fds) {
    return;
  }

  if (!parent.IsObject()) {
    parent.SetObject();
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidjson_loader_append_to_list(ls, src.GetReflection()->GetRepeatedInt32(src, fds, i), doc);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson_loader_mutable_set_member(parent, fds->name(), src.GetReflection()->GetInt32(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          int64_t int_val = src.GetReflection()->GetRepeatedInt64(src, fds, i);
          if (options.convert_large_number_to_string && int_val > std::numeric_limits<int32_t>::max()) {
            char str_val[24] = {0};
            atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
            rapidjson::Value v;
            v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
            rapidjson_loader_append_to_list(ls, std::move(v), doc);
          } else {
            rapidjson_loader_append_to_list(ls, int_val, doc);
          }
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        int64_t int_val = src.GetReflection()->GetInt64(src, fds);
        if (options.convert_large_number_to_string && int_val > std::numeric_limits<int32_t>::max()) {
          char str_val[24] = {0};
          atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
          rapidjson::Value v;
          v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
          rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(v), doc);
        } else {
          rapidjson_loader_mutable_set_member(parent, fds->name(), int_val, doc);
        }
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          uint32_t int_val = src.GetReflection()->GetRepeatedUInt32(src, fds, i);
          if (options.convert_large_number_to_string &&
              int_val > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            char str_val[24] = {0};
            atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
            rapidjson::Value v;
            v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
            rapidjson_loader_append_to_list(ls, std::move(v), doc);
          } else {
            rapidjson_loader_append_to_list(ls, int_val, doc);
          }
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        uint32_t int_val = src.GetReflection()->GetUInt32(src, fds);
        if (options.convert_large_number_to_string &&
            int_val > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          char str_val[24] = {0};
          atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
          rapidjson::Value v;
          v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
          rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(v), doc);
        } else {
          rapidjson_loader_mutable_set_member(parent, fds->name(), int_val, doc);
        }
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          uint64_t int_val = src.GetReflection()->GetRepeatedUInt64(src, fds, i);
          if (options.convert_large_number_to_string &&
              int_val > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            char str_val[24] = {0};
            atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
            rapidjson::Value v;
            v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
            rapidjson_loader_append_to_list(ls, std::move(v), doc);
          } else {
            rapidjson_loader_append_to_list(ls, int_val, doc);
          }
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        uint64_t int_val = src.GetReflection()->GetUInt64(src, fds);
        if (options.convert_large_number_to_string &&
            int_val > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
          char str_val[24] = {0};
          atfw::util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
          rapidjson::Value v;
          v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
          rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(v), doc);
        } else {
          rapidjson_loader_mutable_set_member(parent, fds->name(), int_val, doc);
        }
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      std::string empty;
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidjson::Value v;
          load_field_string_filter(src.GetReflection()->GetRepeatedStringReference(src, fds, i, &empty), v, doc,
                                   options);
          rapidjson_loader_append_to_list(ls, std::move(v), doc);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson::Value v;
        load_field_string_filter(src.GetReflection()->GetStringReference(src, fds, &empty), v, doc, options);
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(v), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      if (fds->is_map()) {
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *msg_desc = fds->message_type();
        if (nullptr == msg_desc) {
          break;
        }
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds = msg_desc->map_key();
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds = msg_desc->map_value();
        if (nullptr == key_fds || nullptr == value_fds) {
          break;
        }

        rapidjson::Value obj;
        obj.SetObject();
        if (obj.IsObject()) {
          rapidjson::Value obj_key;
          rapidjson::Value obj_value;
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedFieldRef<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message> data =
              src.GetReflection()->GetRepeatedFieldRef<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message>(src, fds);
          for (int i = 0; i < data.size(); ++i) {
            load_map_field_item(obj_key, data.Get(i, nullptr), key_fds, doc, options);
            load_map_field_item(obj_value, data.Get(i, nullptr), value_fds, doc, options);
            if (!obj_key.IsNull() && !obj_value.IsNull()) {
              rapidjson::Value map_key_jstring;
              if (obj_key.IsString()) {
                map_key_jstring = std::move(obj_key);
              } else {
                map_key_jstring = load_field_item_to_map_string(data.Get(i, nullptr), key_fds, doc);
              }

              rapidjson::Value::MemberIterator iter = obj.FindMember(map_key_jstring);
              if (iter != obj.MemberEnd()) {
                iter->value.Swap(obj_value);
              } else {
                obj.AddMember(map_key_jstring, obj_value, doc.GetAllocator());
              }
            }
          }
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(obj), doc);

      } else if (fds->is_repeated()) {
        rapidjson::Value ls;
        ls.SetArray();
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedFieldRef<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message> data =
            src.GetReflection()->GetRepeatedFieldRef<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message>(src, fds);
        if (ls.IsArray()) {
          for (int i = 0; i < data.size(); ++i) {
            ls.PushBack(rapidjson::kObjectType, doc.GetAllocator());
            rapidjson_loader_load_from(ls[ls.Size() - 1], doc, data.Get(i, nullptr), options);
          }
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson::Value obj;
        obj.SetObject();
        if (obj.IsObject()) {
          rapidjson_loader_load_from(obj, doc, src.GetReflection()->GetMessage(src, fds), options);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(obj), doc);
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidjson_loader_append_to_list(ls, src.GetReflection()->GetRepeatedDouble(src, fds, i), doc);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson_loader_mutable_set_member(parent, fds->name(), src.GetReflection()->GetDouble(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidjson_loader_append_to_list(ls, src.GetReflection()->GetRepeatedFloat(src, fds, i), doc);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson_loader_mutable_set_member(parent, fds->name(), src.GetReflection()->GetFloat(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidjson_loader_append_to_list(ls, src.GetReflection()->GetRepeatedBool(src, fds, i), doc);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson_loader_mutable_set_member(parent, fds->name(), src.GetReflection()->GetBool(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidjson_loader_append_to_list(ls, src.GetReflection()->GetRepeatedEnumValue(src, fds, i), doc);
        }
        rapidjson_loader_mutable_set_member(parent, fds->name(), std::move(ls), doc);
      } else {
        rapidjson_loader_mutable_set_member(parent, fds->name(), src.GetReflection()->GetEnumValue(src, fds), doc);
      }
      break;
    };
    default: {
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), fds->containing_type()->full_name(),
                 fds->type_name());
      break;
    }
  }
}

static std::string dump_pick_field_string_filter(const rapidjson::Value &val,
                                                 const rapidjson_loader_dump_options &options) {
  if (!val.IsString()) {
    return std::string();
  }

  switch (options.string_mode) {
    case rapidjson_loader_string_mode::kUri:
      return atfw::util::uri::decode_uri(val.GetString(), val.GetStringLength());
    case rapidjson_loader_string_mode::kUriComponent:
      return atfw::util::uri::decode_uri_component(val.GetString(), val.GetStringLength());
    default:
      return std::string(val.GetString(), val.GetStringLength());
  }
}

static void dump_pick_field(const rapidjson::Value &val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                            const rapidjson_loader_dump_options &options) {
  if (nullptr == fds) {
    return;
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      int32_t jval = 0;
      if (val.IsInt()) {
        jval = val.GetInt();
      } else if (val.IsString() && options.convert_number_from_string) {
        atfw::util::string::str2int(jval, val.GetString());
      }
      if (fds->is_repeated()) {
        dst.GetReflection()->AddInt32(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetInt32(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      int64_t jval = 0;
      if (val.IsInt64()) {
        jval = val.GetInt64();
      } else if (val.IsString() && options.convert_number_from_string) {
        atfw::util::string::str2int(jval, val.GetString());
      }
      if (fds->is_repeated()) {
        dst.GetReflection()->AddInt64(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetInt64(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t jval = 0;
      if (val.IsUint()) {
        jval = val.GetUint();
      } else if (val.IsString() && options.convert_number_from_string) {
        atfw::util::string::str2int(jval, val.GetString());
      }
      if (fds->is_repeated()) {
        dst.GetReflection()->AddUInt32(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetUInt32(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t jval = 0;
      if (val.IsUint64()) {
        jval = val.GetUint64();
      } else if (val.IsString() && options.convert_number_from_string) {
        atfw::util::string::str2int(jval, val.GetString());
      }
      if (fds->is_repeated()) {
        dst.GetReflection()->AddUInt64(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetUInt64(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      if (fds->is_repeated()) {
        dst.GetReflection()->AddString(&dst, fds, dump_pick_field_string_filter(val, options));
      } else {
        dst.GetReflection()->SetString(&dst, fds, dump_pick_field_string_filter(val, options));
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      if (!val.IsObject()) {
        // type error
        break;
      }

      rapidjson::Value &jval = const_cast<rapidjson::Value &>(val);
      if (fds->is_repeated()) {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
        if (nullptr != submsg) {
          rapidjson_loader_dump_to(jval, *submsg, options);
        }
      } else {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (nullptr != submsg) {
          rapidjson_loader_dump_to(jval, *submsg, options);
        }
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      double jval = val.IsDouble() ? val.GetDouble() : 0;
      if (fds->is_repeated()) {
        dst.GetReflection()->AddDouble(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetDouble(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      float jval = val.IsFloat() ? val.GetFloat() : 0;
      if (fds->is_repeated()) {
        dst.GetReflection()->AddFloat(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetFloat(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      bool jval = val.IsBool() ? val.GetBool() : false;
      if (fds->is_repeated()) {
        dst.GetReflection()->AddBool(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetBool(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      int jval_num = val.IsInt() ? val.GetInt() : 0;
      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *jval =
          fds->enum_type()->FindValueByNumber(jval_num);
      if (jval == nullptr) {
        // invalid value
        break;
      }
      // fds->enum_type
      if (fds->is_repeated()) {
        dst.GetReflection()->AddEnum(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetEnum(&dst, fds, jval);
      }
      break;
    };
    default: {
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                 fds->type_name());
      break;
    }
  }
}

static void dump_field_item_map(const rapidjson::Value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                const rapidjson_loader_dump_options &options) {
  if (nullptr == fds) {
    return;
  }

  if (!src.IsObject()) {
    return;
  }

  if (ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE != fds->cpp_type()) {
    return;
  }

  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = fds->message_type();
  if (nullptr == desc) {
    return;
  }

  if (!fds->is_map()) {
    return;
  }

  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds = desc->map_key();
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds = desc->map_value();
  if (nullptr == key_fds || nullptr == value_fds) {
    return;
  }

  rapidjson::Value::ConstMemberIterator iter = src.MemberBegin();
  for (; iter != src.MemberEnd(); ++iter) {
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
    if (nullptr == submsg) {
      continue;
    }

    dump_pick_field(iter->name, *submsg, key_fds, options);
    dump_pick_field(iter->value, *submsg, value_fds, options);
  }
}

static void dump_field_item(const rapidjson::Value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                            const rapidjson_loader_dump_options &options) {
  if (nullptr == fds) {
    return;
  }

  if (!src.IsObject()) {
    return;
  }

  auto key = rapidjson::StringRef(fds->name().data(), fds->name().size());
  rapidjson::Value::ConstMemberIterator iter = src.FindMember(key);
  if (iter == src.MemberEnd()) {
    // field not found, just skip
    return;
  }

  const rapidjson::Value &val = iter->value;
  if (val.IsArray() && !fds->is_repeated()) {
    // Type error
    return;
  }

  if (fds->is_repeated()) {
    if (val.IsObject()) {
      dump_field_item_map(val, dst, fds, options);
      return;
    }

    if (!val.IsArray()) {
      // Type error
      return;
    }

    rapidjson::SizeType arrsz = val.Size();
    for (rapidjson::SizeType i = 0; i < arrsz; ++i) {
      dump_pick_field(val[i], dst, fds, options);
    }
  } else {
    if (val.IsArray()) {
      return;
    }

    dump_pick_field(val, dst, fds, options);
  }
}
}  // namespace detail

LIBATAPP_MACRO_API std::string rapidjson_loader_stringify(const rapidjson::Document &doc, size_t more_reserve_size) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    // Stringify the DOM
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string ret;
    ret.reserve(buffer.GetSize() + more_reserve_size + 1);
    ret.assign(buffer.GetString(), buffer.GetSize());
    return ret;
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    return std::string();
  }
#endif
}

LIBATAPP_MACRO_API bool rapidjson_loader_unstringify(rapidjson::Document &doc, const std::string &json) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    doc.Parse(json.c_str(), json.size());
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    return false;
  }
#endif

  if (!doc.IsObject() && !doc.IsArray()) {
    return false;
  }

  return true;
}

LIBATAPP_MACRO_API const char *rapidjson_loader_get_type_name(rapidjson::Type t) {
  switch (t) {
    case rapidjson::kNullType:
      return "null";
    case rapidjson::kFalseType:
      return "boolean(false)";
    case rapidjson::kTrueType:
      return "boolean(true)";
    case rapidjson::kObjectType:
      return "object";
    case rapidjson::kArrayType:
      return "array";
    case rapidjson::kStringType:
      return "string";
    case rapidjson::kNumberType:
      return "number";
    default:
      return "UNKNOWN";
  }
}

LIBATAPP_MACRO_API gsl::span<unsigned char> rapidjson_loader_get_default_shared_buffer() {
  return detail::rapidjson_loader_get_shared_buffer();
}

LIBATAPP_MACRO_API std::string rapidjson_loader_stringify(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                                                          const rapidjson_loader_load_options &options,
                                                          gsl::span<unsigned char> use_buffer) {
  if (use_buffer.data() == nullptr || use_buffer.size() < 8 * sizeof(size_t)) {
    use_buffer = rapidjson_loader_get_default_shared_buffer();
  }

  rapidjson::MemoryPoolAllocator<> allocator{reinterpret_cast<void *>(use_buffer.data()), use_buffer.size()};
  rapidjson::Document doc{&allocator};
  rapidjson_loader_load_from(doc, src, options);
  return rapidjson_loader_stringify(doc);
}

LIBATAPP_MACRO_API bool rapidjson_loader_parse(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst, const std::string &src,
                                               const rapidjson_loader_dump_options &options,
                                               gsl::span<unsigned char> use_buffer) {
  if (use_buffer.data() == nullptr || use_buffer.size() < 8 * sizeof(size_t)) {
    use_buffer = rapidjson_loader_get_default_shared_buffer();
  }

  rapidjson::MemoryPoolAllocator<> allocator{reinterpret_cast<void *>(use_buffer.data()), use_buffer.size()};
  rapidjson::Document doc{&allocator};
  if (!rapidjson_loader_unstringify(doc, src)) {
    return false;
  }

  rapidjson_loader_dump_to(doc, dst, options);
  return true;
}

LIBATAPP_MACRO_API void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                            rapidjson::Value &&val, rapidjson::Document &doc) {
  if (!parent.IsObject()) {
    parent.SetObject();
  }

  rapidjson::Value jkey;
  jkey.SetString(rapidjson::StringRef(key.data(), static_cast<rapidjson::SizeType>(key.size())));

  rapidjson::Value::MemberIterator iter = parent.FindMember(jkey);
  if (iter != parent.MemberEnd()) {
    iter->value.Swap(val);
  } else {
    rapidjson::Value k;
    rapidjson::Value v;
    k.SetString(key.data(), static_cast<rapidjson::SizeType>(key.size()), doc.GetAllocator());
    v.Swap(val);
    parent.AddMember(k, v, doc.GetAllocator());
  }
}

LIBATAPP_MACRO_API void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                            const rapidjson::Value &val, rapidjson::Document &doc) {
  if (!parent.IsObject()) {
    parent.SetObject();
  }

  rapidjson::Value jkey;
  jkey.SetString(rapidjson::StringRef(key.data(), static_cast<rapidjson::SizeType>(key.size())));

  rapidjson::Value::MemberIterator iter = parent.FindMember(jkey);
  if (iter != parent.MemberEnd()) {
    iter->value.CopyFrom(val, doc.GetAllocator());
  } else {
    rapidjson::Value k;
    rapidjson::Value v;
    k.SetString(key.data(), static_cast<rapidjson::SizeType>(key.size()), doc.GetAllocator());
    v.CopyFrom(val, doc.GetAllocator());
    parent.AddMember(k, v, doc.GetAllocator());
  }
}

LIBATAPP_MACRO_API void rapidjson_loader_mutable_set_member(rapidjson::Value &parent, gsl::string_view key,
                                                            gsl::string_view val, rapidjson::Document &doc) {
  rapidjson::Value v;
  v.SetString(val.data(), static_cast<rapidjson::SizeType>(val.size()), doc.GetAllocator());
  rapidjson_loader_mutable_set_member(parent, key, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidjson_loader_append_to_list(rapidjson::Value &list_parent, gsl::string_view val,
                                                        rapidjson::Document &doc) {
  rapidjson::Value v;
  v.SetString(val.data(), static_cast<rapidjson::SizeType>(val.size()), doc.GetAllocator());
  rapidjson_loader_append_to_list(list_parent, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidjson_loader_dump_to(const rapidjson::Document &src,
                                                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                 const rapidjson_loader_dump_options &options) {
  if (src.IsObject()) {
    rapidjson::Value &srcobj = const_cast<rapidjson::Document &>(src);
    rapidjson_loader_dump_to(srcobj, dst, options);
  }
}

LIBATAPP_MACRO_API void rapidjson_loader_load_from(rapidjson::Document &dst,
                                                   const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                                                   const rapidjson_loader_load_options &options) {
  if (!dst.IsObject()) {
    dst.SetObject();
  }
  rapidjson::Value &root = dst;
  rapidjson_loader_load_from(root, dst, src, options);
}

LIBATAPP_MACRO_API void rapidjson_loader_dump_to(const rapidjson::Value &src,
                                                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                 const rapidjson_loader_dump_options &options) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
    return;
  }

  for (int i = 0; i < desc->field_count(); ++i) {
    detail::dump_field_item(src, dst, desc->field(i), options);
  }
}

LIBATAPP_MACRO_API void rapidjson_loader_load_from(rapidjson::Value &dst, rapidjson::Document &doc,
                                                   const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &src,
                                                   const rapidjson_loader_load_options &options) {
  if (options.reserve_empty) {
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = src.GetDescriptor();
    if (nullptr == desc) {
      return;
    }

    for (int i = 0; i < desc->field_count(); ++i) {
      detail::load_field_item(dst, src, desc->field(i), doc, options);
    }
  } else {
    std::vector<const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *> fields_with_data;
    src.GetReflection()->ListFields(src, &fields_with_data);
    for (size_t i = 0; i < fields_with_data.size(); ++i) {
      detail::load_field_item(dst, src, fields_with_data[i], doc, options);
    }
  }
}
LIBATAPP_MACRO_NAMESPACE_END
