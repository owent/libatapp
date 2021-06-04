#include <config/compiler/protobuf_prefix.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#include <google/protobuf/repeated_field.h>

#include <config/compiler/protobuf_suffix.h>

#include <common/string_oprs.h>
#include <string/tquerystring.h>

#include <atframe/atapp_conf_rapidjson.h>

#include <assert.h>

#include <numeric>
#include <vector>

#if defined(max)
#  undef max
#endif

#if defined(GetMessage)
#  undef GetMessage
#endif

namespace atapp {
namespace detail {
static void load_field_string_filter(const std::string& input, rapidjson::Value& output, rapidjson::Document& doc,
                                     const rapidsjon_loader_load_options& options) {
  switch (options.string_mode) {
    case rapidsjon_loader_string_mode::URI: {
      std::string strv = util::uri::encode_uri(input.c_str(), input.size());
      output.SetString(strv.c_str(), static_cast<rapidjson::SizeType>(strv.size()), doc.GetAllocator());
      break;
    }
    case rapidsjon_loader_string_mode::URI_COMPONENT: {
      std::string strv = util::uri::encode_uri_component(input.c_str(), input.size());
      output.SetString(strv.c_str(), static_cast<rapidjson::SizeType>(strv.size()), doc.GetAllocator());
      break;
    }
    default: {
      output.SetString(input.c_str(), static_cast<rapidjson::SizeType>(input.size()), doc.GetAllocator());
      break;
    }
  }
}

static void load_field_item(rapidjson::Value& parent, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& src,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds, rapidjson::Document& doc,
                            const rapidsjon_loader_load_options& options) {
  if (NULL == fds) {
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
          rapidsjon_loader_append_to_list(ls, src.GetReflection()->GetRepeatedInt32(src, fds, i), doc);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), src.GetReflection()->GetInt32(src, fds), doc);
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
            util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
            rapidjson::Value v;
            v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
            rapidsjon_loader_append_to_list(ls, std::move(v), doc);
          } else {
            rapidsjon_loader_append_to_list(ls, int_val, doc);
          }
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        int64_t int_val = src.GetReflection()->GetInt64(src, fds);
        if (options.convert_large_number_to_string && int_val > std::numeric_limits<int32_t>::max()) {
          char str_val[24] = {0};
          util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
          rapidjson::Value v;
          v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
          rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(v), doc);
        } else {
          rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), int_val, doc);
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
            util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
            rapidjson::Value v;
            v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
            rapidsjon_loader_append_to_list(ls, std::move(v), doc);
          } else {
            rapidsjon_loader_append_to_list(ls, int_val, doc);
          }
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        uint32_t int_val = src.GetReflection()->GetUInt32(src, fds);
        if (options.convert_large_number_to_string &&
            int_val > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
          char str_val[24] = {0};
          util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
          rapidjson::Value v;
          v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
          rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(v), doc);
        } else {
          rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), int_val, doc);
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
            util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
            rapidjson::Value v;
            v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
            rapidsjon_loader_append_to_list(ls, std::move(v), doc);
          } else {
            rapidsjon_loader_append_to_list(ls, int_val, doc);
          }
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        uint64_t int_val = src.GetReflection()->GetUInt64(src, fds);
        if (options.convert_large_number_to_string &&
            int_val > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
          char str_val[24] = {0};
          util::string::int2str(str_val, sizeof(str_val) - 1, int_val);
          rapidjson::Value v;
          v.SetString(str_val, static_cast<rapidjson::SizeType>(strlen(str_val)), doc.GetAllocator());
          rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(v), doc);
        } else {
          rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), int_val, doc);
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
          rapidsjon_loader_append_to_list(ls, std::move(v), doc);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidjson::Value v;
        load_field_string_filter(src.GetReflection()->GetStringReference(src, fds, &empty), v, doc, options);
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(v), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      if (fds->is_repeated()) {
        rapidjson::Value ls;
        ls.SetArray();
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::RepeatedFieldRef<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message> data =
            src.GetReflection()->GetRepeatedFieldRef<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message>(src, fds);
        if (ls.IsArray()) {
          for (int i = 0; i < data.size(); ++i) {
            ls.PushBack(rapidjson::kObjectType, doc.GetAllocator());
            rapidsjon_loader_load_from(ls[ls.Size() - 1], doc, data.Get(i, nullptr), options);
          }
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidjson::Value obj;
        obj.SetObject();
        if (obj.IsObject()) {
          rapidsjon_loader_load_from(obj, doc, src.GetReflection()->GetMessage(src, fds), options);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(obj), doc);
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidsjon_loader_append_to_list(ls, src.GetReflection()->GetRepeatedDouble(src, fds, i), doc);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), src.GetReflection()->GetDouble(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidsjon_loader_append_to_list(ls, src.GetReflection()->GetRepeatedFloat(src, fds, i), doc);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), src.GetReflection()->GetFloat(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidsjon_loader_append_to_list(ls, src.GetReflection()->GetRepeatedBool(src, fds, i), doc);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), src.GetReflection()->GetBool(src, fds), doc);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      if (fds->is_repeated()) {
        int len = src.GetReflection()->FieldSize(src, fds);
        rapidjson::Value ls;
        ls.SetArray();
        for (int i = 0; i < len; ++i) {
          rapidsjon_loader_append_to_list(ls, src.GetReflection()->GetRepeatedEnumValue(src, fds, i), doc);
        }
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), std::move(ls), doc);
      } else {
        rapidsjon_loader_mutable_set_member(parent, fds->name().c_str(), src.GetReflection()->GetEnumValue(src, fds),
                                            doc);
      }
      break;
    };
    default: {
      WLOGERROR("%s in ConstSettings with type=%s is not supported now", fds->name().c_str(), fds->type_name());
      break;
    }
  }
}

static std::string dump_pick_field_string_filter(const rapidjson::Value& val,
                                                 const rapidsjon_loader_dump_options& options) {
  if (!val.IsString()) {
    return std::string();
  }

  switch (options.string_mode) {
    case rapidsjon_loader_string_mode::URI:
      return util::uri::decode_uri(val.GetString(), val.GetStringLength());
    case rapidsjon_loader_string_mode::URI_COMPONENT:
      return util::uri::decode_uri_component(val.GetString(), val.GetStringLength());
    default:
      return std::string(val.GetString(), val.GetStringLength());
  }
}

static void dump_pick_field(const rapidjson::Value& val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds,
                            const rapidsjon_loader_dump_options& options) {
  if (NULL == fds) {
    return;
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      int32_t jval = 0;
      if (val.IsInt()) {
        jval = val.GetInt();
      } else if (val.IsString() && options.convert_number_from_string) {
        util::string::str2int(jval, val.GetString());
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
        util::string::str2int(jval, val.GetString());
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
        util::string::str2int(jval, val.GetString());
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
        util::string::str2int(jval, val.GetString());
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

      rapidjson::Value& jval = const_cast<rapidjson::Value&>(val);
      if (fds->is_repeated()) {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message* submsg = dst.GetReflection()->AddMessage(&dst, fds);
        if (NULL != submsg) {
          rapidsjon_loader_dump_to(jval, *submsg, options);
        }
      } else {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message* submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (NULL != submsg) {
          rapidsjon_loader_dump_to(jval, *submsg, options);
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
      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor* jval =
          fds->enum_type()->FindValueByNumber(jval_num);
      if (jval == NULL) {
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
      WLOGERROR("%s in %s with type=%s is not supported now", fds->name().c_str(),
                dst.GetDescriptor()->full_name().c_str(), fds->type_name());
      break;
    }
  }
}

static void dump_field_item(const rapidjson::Value& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds,
                            const rapidsjon_loader_dump_options& options) {
  if (NULL == fds) {
    return;
  }

  if (!src.IsObject()) {
    return;
  }

  rapidjson::Value::ConstMemberIterator iter = src.FindMember(fds->name().c_str());
  if (iter == src.MemberEnd()) {
    // field not found, just skip
    return;
  }

  const rapidjson::Value& val = iter->value;
  if (val.IsArray() && !fds->is_repeated()) {
    // Type error
    return;
  }

  if (fds->is_repeated()) {
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

LIBATAPP_MACRO_API std::string rapidsjon_loader_stringify(const rapidjson::Document& doc, size_t more_reserve_size) {
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

LIBATAPP_MACRO_API bool rapidsjon_loader_unstringify(rapidjson::Document& doc, const std::string& json) {
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

LIBATAPP_MACRO_API const char* rapidsjon_loader_get_type_name(rapidjson::Type t) {
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

LIBATAPP_MACRO_API std::string rapidsjon_loader_stringify(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& src,
                                                          const rapidsjon_loader_load_options& options) {
  rapidjson::Document doc;
  rapidsjon_loader_load_from(doc, src, options);
  return rapidsjon_loader_stringify(doc);
}

LIBATAPP_MACRO_API bool rapidsjon_loader_parse(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst, const std::string& src,
                                               const rapidsjon_loader_dump_options& options) {
  rapidjson::Document doc;
  if (!rapidsjon_loader_unstringify(doc, src)) {
    return false;
  }

  rapidsjon_loader_dump_to(doc, dst, options);
  return true;
}

LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value& parent, const char* key,
                                                            rapidjson::Value&& val, rapidjson::Document& doc) {
  if (!parent.IsObject()) {
    parent.SetObject();
  }

  rapidjson::Value::MemberIterator iter = parent.FindMember(key);
  if (iter != parent.MemberEnd()) {
    iter->value.Swap(val);
  } else {
    rapidjson::Value k;
    rapidjson::Value v;
    k.SetString(key, doc.GetAllocator());
    v.Swap(val);
    parent.AddMember(k, v, doc.GetAllocator());
  }
}

LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value& parent, const char* key,
                                                            const rapidjson::Value& val, rapidjson::Document& doc) {
  if (!parent.IsObject()) {
    parent.SetObject();
  }

  rapidjson::Value::MemberIterator iter = parent.FindMember(key);
  if (iter != parent.MemberEnd()) {
    iter->value.CopyFrom(val, doc.GetAllocator());
  } else {
    rapidjson::Value k;
    rapidjson::Value v;
    k.SetString(key, doc.GetAllocator());
    v.CopyFrom(val, doc.GetAllocator());
    parent.AddMember(k, v, doc.GetAllocator());
  }
}

LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value& parent, const char* key,
                                                            const std::string& val, rapidjson::Document& doc) {
  rapidjson::Value v;
  v.SetString(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), doc.GetAllocator());
  rapidsjon_loader_mutable_set_member(parent, key, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value& parent, const char* key, std::string& val,
                                                            rapidjson::Document& doc) {
  rapidjson::Value v;
  v.SetString(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), doc.GetAllocator());
  rapidsjon_loader_mutable_set_member(parent, key, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidsjon_loader_mutable_set_member(rapidjson::Value& parent, const char* key, const char* val,
                                                            rapidjson::Document& doc) {
  rapidjson::Value v;
  v.SetString(val, doc.GetAllocator());
  rapidsjon_loader_mutable_set_member(parent, key, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidsjon_loader_append_to_list(rapidjson::Value& list_parent, const std::string& val,
                                                        rapidjson::Document& doc) {
  rapidjson::Value v;
  v.SetString(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), doc.GetAllocator());
  rapidsjon_loader_append_to_list(list_parent, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidsjon_loader_append_to_list(rapidjson::Value& list_parent, std::string& val,
                                                        rapidjson::Document& doc) {
  rapidjson::Value v;
  v.SetString(val.c_str(), static_cast<rapidjson::SizeType>(val.size()), doc.GetAllocator());
  rapidsjon_loader_append_to_list(list_parent, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidsjon_loader_append_to_list(rapidjson::Value& list_parent, const char* val,
                                                        rapidjson::Document& doc) {
  rapidjson::Value v;
  v.SetString(val, doc.GetAllocator());
  rapidsjon_loader_append_to_list(list_parent, std::move(v), doc);
}

LIBATAPP_MACRO_API void rapidsjon_loader_dump_to(const rapidjson::Document& src,
                                                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst,
                                                 const rapidsjon_loader_dump_options& options) {
  if (src.IsObject()) {
    rapidjson::Value& srcobj = const_cast<rapidjson::Document&>(src);
    rapidsjon_loader_dump_to(srcobj, dst, options);
  }
}

LIBATAPP_MACRO_API void rapidsjon_loader_load_from(rapidjson::Document& dst,
                                                   const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& src,
                                                   const rapidsjon_loader_load_options& options) {
  if (!dst.IsObject()) {
    dst.SetObject();
  }
  rapidjson::Value& root = dst;
  rapidsjon_loader_load_from(root, dst, src, options);
}

LIBATAPP_MACRO_API void rapidsjon_loader_dump_to(const rapidjson::Value& src,
                                                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst,
                                                 const rapidsjon_loader_dump_options& options) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor* desc = dst.GetDescriptor();
  if (NULL == desc) {
    return;
  }

  for (int i = 0; i < desc->field_count(); ++i) {
    detail::dump_field_item(src, dst, desc->field(i), options);
  }
}

LIBATAPP_MACRO_API void rapidsjon_loader_load_from(rapidjson::Value& dst, rapidjson::Document& doc,
                                                   const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& src,
                                                   const rapidsjon_loader_load_options& options) {
  if (options.reserve_empty) {
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor* desc = src.GetDescriptor();
    if (NULL == desc) {
      return;
    }

    for (int i = 0; i < desc->field_count(); ++i) {
      detail::load_field_item(dst, src, desc->field(i), doc, options);
    }
  } else {
    std::vector<const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor*> fields_with_data;
    src.GetReflection()->ListFields(src, &fields_with_data);
    for (size_t i = 0; i < fields_with_data.size(); ++i) {
      detail::load_field_item(dst, src, fields_with_data[i], doc, options);
    }
  }
}
}  // namespace atapp
