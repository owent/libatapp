#include <algorithm>
#include <limits>
#include <sstream>

#include "libatbus.h"

#include <common/string_oprs.h>
#include <config/atframe_utils_build_feature.h>
#include <config/ini_loader.h>
#include <log/log_wrapper.h>
#include <time/time_utility.h>

#include <config/compiler/protobuf_prefix.h>

#include "yaml-cpp/yaml.h"

#include <google/protobuf/duration.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>
#include <google/protobuf/timestamp.pb.h>

#include <config/compiler/protobuf_suffix.h>

#include <atframe/atapp_conf.h>

#ifdef GetMessage
#  undef GetMessage
#endif

#ifdef max
#  undef max
#endif

#ifdef min
#  undef min
#endif

namespace atapp {
namespace detail {
static const char *skip_space(const char *str) {
  while (str && *str) {
    if (::util::string::is_space(*str)) {
      ++str;
      continue;
    }
    break;
  }

  return str;
}

template <typename TINT>
static const char *pick_number(TINT &out, const char *str) {
  out = 0;
  if (NULL == str || !(*str)) {
    return str;
  }

  // negative
  bool is_negative = false;
  while (*str && *str == '-') {
    is_negative = !is_negative;
    ++str;
  }

  if (!(*str)) {
    return str;
  }

  // dec only
  if ('0' == str[0] && 'x' == util::string::tolower(str[1])) {  // hex
    str += 2;
    while (str && *str) {
      char c = util::string::tolower(*str);
      if (c >= '0' && c <= '9') {
        out <<= 4;
        out += c - '0';
        ++str;
      } else if (c >= 'a' && c <= 'f') {
        out <<= 4;
        out += c - 'a' + 10;
        ++str;
      } else {
        break;
      }
    }
  } else if ('\\' == str[0]) {
    ++str;
    while (str && *str >= '0' && *str <= '7') {
      out <<= 3;
      out += *str - '0';
      ++str;
    }
  } else {
    while (str && *str >= '0' && *str <= '9') {
      out *= 10;
      out += *str - '0';
      ++str;
    }
  }

  if (is_negative) {
    out = (~out) + 1;
  }

  return str;
}

/*
static bool protobuf_field_cmp_fn(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* l, const
ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* r) { int lv = (NULL == l)? 0: l->number(); int rv = (NULL == r)? 0:
r->number(); return lv < rv;
}
*/

static void pick_const_data(const std::string &value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &dur) {
  dur.set_seconds(0);
  dur.set_nanos(0);

  int64_t tm_val = 0;
  const char *word_begin = value.c_str();
  word_begin = skip_space(word_begin);
  word_begin = pick_number(tm_val, word_begin);
  word_begin = skip_space(word_begin);

  const char *word_end = value.c_str() + value.size();
  std::string unit;
  if (word_begin && word_end && word_end > word_begin) {
    unit.assign(word_begin, word_end);
    std::transform(unit.begin(), unit.end(), unit.begin(), ::util::string::tolower<char>);
  }

  bool fallback = true;
  do {
    if (unit.empty() || unit == "s" || unit == "sec" || unit == "second" || unit == "seconds") {
      break;
    }

    if (unit == "ms" || unit == "millisecond" || unit == "milliseconds") {
      fallback = false;
      dur.set_seconds(tm_val / 1000);
      dur.set_nanos((tm_val % 1000) * 1000000);
      break;
    }

    if (unit == "us" || unit == "microsecond" || unit == "microseconds") {
      fallback = false;
      dur.set_seconds(tm_val / 1000000);
      dur.set_nanos((tm_val % 1000000) * 1000);
      break;
    }

    if (unit == "ns" || unit == "nanosecond" || unit == "nanoseconds") {
      fallback = false;
      dur.set_seconds(tm_val / 1000000000);
      dur.set_nanos(tm_val % 1000000000);
      break;
    }

    if (unit == "m" || unit == "minute" || unit == "minutes") {
      fallback = false;
      dur.set_seconds(tm_val * 60);
      break;
    }

    if (unit == "h" || unit == "hour" || unit == "hours") {
      fallback = false;
      dur.set_seconds(tm_val * 3600);
      break;
    }

    if (unit == "d" || unit == "day" || unit == "days") {
      fallback = false;
      dur.set_seconds(tm_val * 3600 * 24);
      break;
    }

    if (unit == "w" || unit == "week" || unit == "weeks") {
      fallback = false;
      dur.set_seconds(tm_val * 3600 * 24 * 7);
      break;
    }

  } while (false);

  // fallback to second
  if (fallback) {
    dur.set_seconds(tm_val);
  }
}

static void pick_const_data(const std::string &value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &timepoint) {
  timepoint.set_seconds(0);
  timepoint.set_nanos(0);

  const char *word_begin = value.c_str();
  word_begin = skip_space(word_begin);

  struct tm t;
  memset(&t, 0, sizeof(t));

  // year
  {
    word_begin = pick_number(t.tm_year, word_begin);
    word_begin = skip_space(word_begin);
    if (*word_begin == '-') {
      ++word_begin;
      word_begin = skip_space(word_begin);
    }
    t.tm_year -= 1900;  // years since 1900
  }
  // month
  {
    word_begin = pick_number(t.tm_mon, word_begin);
    word_begin = skip_space(word_begin);
    if (*word_begin == '-') {
      ++word_begin;
      word_begin = skip_space(word_begin);
    }

    --t.tm_mon;  // [0, 11]
  }
  // day
  {
    word_begin = pick_number(t.tm_mday, word_begin);
    word_begin = skip_space(word_begin);
    if (*word_begin == 'T') {  // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
      ++word_begin;
      word_begin = skip_space(word_begin);
    }
  }

  // tm_hour
  {
    word_begin = pick_number(t.tm_hour, word_begin);
    word_begin = skip_space(word_begin);
    if (*word_begin == ':') {  // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
      ++word_begin;
      word_begin = skip_space(word_begin);
    }
  }

  // tm_min
  {
    word_begin = pick_number(t.tm_min, word_begin);
    word_begin = skip_space(word_begin);
    if (*word_begin == ':') {  // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
      ++word_begin;
      word_begin = skip_space(word_begin);
    }
  }

  // tm_sec
  {
    word_begin = pick_number(t.tm_sec, word_begin);
    word_begin = skip_space(word_begin);
  }

  time_t res = mktime(&t);

  if (*word_begin == 'Z') {  // UTC timezone
    res -= ::util::time::time_utility::get_sys_zone_offset();
  } else if (*word_begin == '+') {
    res -= ::util::time::time_utility::get_sys_zone_offset();
    time_t offset = 0;
    word_begin = pick_number(offset, word_begin + 1);
    res -= offset * 60;
    if (*word_begin && ':' == *word_begin) {
      pick_number(offset, word_begin + 1);
      res -= offset;
    }
    timepoint.set_seconds(timepoint.seconds() - offset);
  } else if (*word_begin == '-') {
    res -= ::util::time::time_utility::get_sys_zone_offset();
    time_t offset = 0;
    word_begin = pick_number(offset, word_begin + 1);
    res += offset * 60;
    if (*word_begin && ':' == *word_begin) {
      pick_number(offset, word_begin + 1);
      res += offset;
    }
  }

  timepoint.set_seconds(res);
}

static inline void dump_pick_field_min(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out) {
  out.set_seconds(std::numeric_limits<int64_t>::min());
  out.set_nanos(std::numeric_limits<int32_t>::min());
}

static inline void dump_pick_field_min(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out) {
  out.set_seconds(std::numeric_limits<int64_t>::min());
  out.set_nanos(std::numeric_limits<int32_t>::min());
}

template <class Ty>
static inline void dump_pick_field_min(Ty &out) {
  out = std::numeric_limits<Ty>::min();
}

static inline void dump_pick_field_max(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out) {
  out.set_seconds(std::numeric_limits<int64_t>::max());
  out.set_nanos(std::numeric_limits<int32_t>::max());
}

static inline void dump_pick_field_max(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out) {
  out.set_seconds(std::numeric_limits<int64_t>::max());
  out.set_nanos(std::numeric_limits<int32_t>::max());
}

template <class Ty>
static inline void dump_pick_field_max(Ty &out) {
  out = std::numeric_limits<Ty>::max();
}

static inline void dump_pick_field_from_str(float &out, const std::string &input, bool) {
  if (input.empty()) {
    out = 0.0f;
    return;
  }
  char *end;
  out = static_cast<float>(strtod(input.c_str(), &end));
}

static inline void dump_pick_field_from_str(double &out, const std::string &input, bool) {
  if (input.empty()) {
    out = 0.0;
    return;
  }
  char *end;
  out = strtod(input.c_str(), &end);
}

static inline void dump_pick_field_from_str(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out, const std::string &input,
                                            bool) {
  if (input.empty()) {
    out.set_seconds(0);
    out.set_nanos(0);
    return;
  }
  pick_const_data(input, out);
}
static inline void dump_pick_field_from_str(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out, const std::string &input,
                                            bool) {
  if (input.empty()) {
    out.set_seconds(0);
    out.set_nanos(0);
    return;
  }
  pick_const_data(input, out);
}

template <class Ty, size_t TSIZE>
struct scale_size_mode_t;

template <class Ty>
struct scale_size_mode_t<Ty, 1> {
  static inline bool to_bytes(Ty &out, char c) { return false; }
};

template <class Ty>
struct scale_size_mode_t<Ty, 2> {
  static inline bool to_bytes(Ty &out, char c) {
    if ('k' == c) {
      out <<= 10;
      return true;
    }
    return false;
  }
};

template <class Ty>
struct scale_size_mode_t<Ty, 4> {
  static inline bool to_bytes(Ty &out, char c) {
    if (scale_size_mode_t<Ty, 2>::to_bytes(out, c)) {
      return true;
    }

    if ('m' == c) {
      out <<= 20;
      return true;
    }

    if ('g' == c) {
      out <<= 30;
      return true;
    }

    return false;
  }
};

template <class Ty>
struct scale_size_mode_t<Ty, 8> {
  static inline bool to_bytes(Ty &out, char c) {
    if (scale_size_mode_t<Ty, 4>::to_bytes(out, c)) {
      return true;
    }

    if ('t' == c) {
      out <<= 40;
      return true;
    }

    if ('p' == c) {
      out <<= 50;
      return true;
    }

    return false;
  }
};

template <class Ty, size_t>
struct scale_size_mode_t {
  static inline bool to_bytes(Ty &out, char c) { return scale_size_mode_t<Ty, 8>::to_bytes(out, c); }
};

template <class Ty>
static inline void dump_pick_field_from_str(Ty &out, const std::string &input, bool size_mode) {
  if (input.empty()) {
    out = 0;
    return;
  }
  const char *left = util::string::str2int(out, input.c_str());

  if (size_mode) {
    while (left && *left && util::string::is_space(*left)) {
      ++left;
    }

    if (left) {
      scale_size_mode_t<Ty, sizeof(Ty)>::to_bytes(out, util::string::tolower(*left));
    }
  }
}

static inline bool dump_pick_field_less(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &l,
                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &r) {
  if (l.seconds() != r.seconds()) {
    return l.seconds() < r.seconds();
  }

  return l.nanos() < r.nanos();
}

static inline bool dump_pick_field_less(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &l,
                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &r) {
  if (l.seconds() != r.seconds()) {
    return l.seconds() < r.seconds();
  }

  return l.nanos() < r.nanos();
}

template <class Ty>
static inline bool dump_pick_field_less(const Ty &l, const Ty &r) {
  return l < r;
}

template <class TRET>
static TRET dump_pick_field_with_extensions(const util::config::ini_value &val,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                            size_t index) {
  TRET value, min_value, max_value;
  dump_pick_field_min(min_value);
  dump_pick_field_max(max_value);
  if (val.as_cpp_string(index).empty()) {
    if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
      const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
      const std::string &default_str = meta.default_value();
      dump_pick_field_from_str(value, default_str, meta.size_mode());
    } else {
      dump_pick_field_from_str(value, std::string(), false);
    }
  } else {
    if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
      const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
      dump_pick_field_from_str(value, val.as_cpp_string(index), meta.size_mode());
    } else {
      dump_pick_field_from_str(value, val.as_cpp_string(index), false);
    }
  }
  if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
    const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
    const std::string &min_val_str = meta.min_value();
    const std::string &max_val_str = meta.max_value();
    if (!min_val_str.empty()) {
      dump_pick_field_from_str(min_value, min_val_str, meta.size_mode());
    }
    if (!max_val_str.empty()) {
      dump_pick_field_from_str(max_value, max_val_str, meta.size_mode());
    }
  }

  if (dump_pick_field_less(value, min_value)) {
    value = min_value;
  }
  if (dump_pick_field_less(max_value, value)) {
    value = max_value;
  }

  return value;
}

static void dump_pick_field(const util::config::ini_value &val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds, size_t index) {
  if (NULL == fds) {
    return;
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      int32_t value = dump_pick_field_with_extensions<int32_t>(val, fds, index);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddInt32(&dst, fds, value);
      } else {
        dst.GetReflection()->SetInt32(&dst, fds, value);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      int64_t value = dump_pick_field_with_extensions<int64_t>(val, fds, index);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddInt64(&dst, fds, value);
      } else {
        dst.GetReflection()->SetInt64(&dst, fds, value);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t value = dump_pick_field_with_extensions<uint32_t>(val, fds, index);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddUInt32(&dst, fds, value);
      } else {
        dst.GetReflection()->SetUInt32(&dst, fds, value);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t value = dump_pick_field_with_extensions<uint64_t>(val, fds, index);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddUInt64(&dst, fds, value);
      } else {
        dst.GetReflection()->SetUInt64(&dst, fds, value);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      const std::string *value = &val.as_cpp_string(index);
      if (NULL == value || value->empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          value = &default_str;
        }
      }
      if (NULL != value) {
        if (fds->is_repeated()) {
          dst.GetReflection()->AddString(&dst, fds, *value);
        } else {
          dst.GetReflection()->SetString(&dst, fds, *value);
        }
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      // special message
      if (fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration value =
            dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>(val, fds, index);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddMessage(&dst, fds)->CopyFrom(value);
        } else {
          dst.GetReflection()->MutableMessage(&dst, fds)->CopyFrom(value);
        }
        break;
      } else if (fds->message_type()->full_name() ==
                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp value =
            dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds, index);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddMessage(&dst, fds)->CopyFrom(value);
        } else {
          dst.GetReflection()->MutableMessage(&dst, fds)->CopyFrom(value);
        }
        break;
      }

      if (fds->is_repeated()) {
        // repeated message is unpack by PARENT.0.field = XXX
        for (uint32_t j = 0;; ++j) {
          std::string idx = LOG_WRAPPER_FWAPI_FORMAT("{}", j);
          util::config::ini_value::node_type::const_iterator idx_iter = val.get_children().find(idx);
          if (idx_iter == val.get_children().end() || !idx_iter->second) {
            break;
          }

          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
          if (NULL != submsg) {
            ini_loader_dump_to(*idx_iter->second, *submsg);
          }
        }
        break;
      } else {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (NULL != submsg) {
          ini_loader_dump_to(val, *submsg);
        }
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      double value = dump_pick_field_with_extensions<double>(val, fds, index);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddDouble(&dst, fds, value);
      } else {
        dst.GetReflection()->SetDouble(&dst, fds, value);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      float value = dump_pick_field_with_extensions<float>(val, fds, index);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddFloat(&dst, fds, value);
      } else {
        dst.GetReflection()->SetFloat(&dst, fds, value);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      bool jval = true;
      std::string trans;
      if (val.as_cpp_string(index).empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          trans = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
        }
      } else {
        trans = val.as_cpp_string(index);
      }
      std::transform(trans.begin(), trans.end(), trans.begin(), util::string::tolower<char>);

      if ("0" == trans || "false" == trans || "no" == trans || "disable" == trans || "disabled" == trans ||
          "" == trans) {
        jval = false;
      }

      if (fds->is_repeated()) {
        dst.GetReflection()->AddBool(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetBool(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      const std::string *value = &val.as_cpp_string(index);
      if (NULL == value || value->empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          value = &default_str;
        }
      }

      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *jval = NULL;
      if (NULL == value || value->empty() || ((*value)[0] >= '0' && (*value)[0] <= '9')) {
        jval = fds->enum_type()->FindValueByNumber(val.as_int32(index));
      } else {
        jval = fds->enum_type()->FindValueByName(*value);
      }

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
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                 fds->type_name());
      break;
    }
  }
}

static void dump_field_item(const util::config::ini_value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (NULL == fds) {
    return;
  }

  util::config::ini_value::node_type::const_iterator child_iter = src.get_children().find(fds->name());
  // skip if not found, just skip
  if (child_iter == src.get_children().end()) {
    return;
  }

  if (!child_iter->second) {
    return;
  }

  if (fds->is_repeated() && fds->cpp_type() != ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE) {
    size_t arrsz = child_iter->second->size();
    for (size_t i = 0; i < arrsz; ++i) {
      dump_pick_field(*child_iter->second, dst, fds, i);
    }
  } else {
    dump_pick_field(*child_iter->second, dst, fds, 0);
  }
}

template <class TRET>
static TRET dump_pick_field_with_extensions(const YAML::Node &val,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  TRET value, min_value, max_value;
  dump_pick_field_min(min_value);
  dump_pick_field_max(max_value);
  if (!val.IsScalar() || val.Scalar().empty()) {
    if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
      const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
      const std::string &default_str = meta.default_value();
      dump_pick_field_from_str(value, default_str, meta.size_mode());
    } else {
      dump_pick_field_from_str(value, std::string(), false);
    }
  } else {
    if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
      const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
      dump_pick_field_from_str(value, val.Scalar(), meta.size_mode());
    } else {
      dump_pick_field_from_str(value, val.Scalar(), false);
    }
  }
  if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
    const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
    const std::string &min_val_str = meta.min_value();
    const std::string &max_val_str = meta.max_value();
    if (!min_val_str.empty()) {
      dump_pick_field_from_str(min_value, min_val_str, meta.size_mode());
    }
    if (!max_val_str.empty()) {
      dump_pick_field_from_str(max_value, max_val_str, meta.size_mode());
    }
  }

  if (dump_pick_field_less(value, min_value)) {
    value = min_value;
  }
  if (dump_pick_field_less(max_value, value)) {
    value = max_value;
  }

  return value;
}

static void dump_message_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst);
static void dump_pick_field(const YAML::Node &val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (NULL == fds) {
    return;
  }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif

    if (!val) {
      return;
    }
    if (val.IsNull() || val.IsSequence()) {
      return;
    }

    switch (fds->cpp_type()) {
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
        int32_t value = dump_pick_field_with_extensions<int32_t>(val, fds);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddInt32(&dst, fds, value);
        } else {
          dst.GetReflection()->SetInt32(&dst, fds, value);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
        int64_t value = dump_pick_field_with_extensions<int64_t>(val, fds);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddInt64(&dst, fds, value);
        } else {
          dst.GetReflection()->SetInt64(&dst, fds, value);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
        uint32_t value = dump_pick_field_with_extensions<uint32_t>(val, fds);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddUInt32(&dst, fds, value);
        } else {
          dst.GetReflection()->SetUInt32(&dst, fds, value);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
        uint64_t value = dump_pick_field_with_extensions<uint64_t>(val, fds);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddUInt64(&dst, fds, value);
        } else {
          dst.GetReflection()->SetUInt64(&dst, fds, value);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
        if (!val.IsScalar()) {
          break;
        }

        const std::string *value = &val.Scalar();
        if (NULL == value || value->empty()) {
          if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
            const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
            value = &default_str;
          }
        }
        if (NULL != value) {
          if (fds->is_repeated()) {
            dst.GetReflection()->AddString(&dst, fds, val.Scalar());
          } else {
            dst.GetReflection()->SetString(&dst, fds, val.Scalar());
          }
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
        // special message
        if (val.IsScalar()) {
          if (fds->message_type()->full_name() ==
              ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration value =
                dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>(val, fds);
            if (fds->is_repeated()) {
              dst.GetReflection()->AddMessage(&dst, fds)->CopyFrom(value);
            } else {
              dst.GetReflection()->MutableMessage(&dst, fds)->CopyFrom(value);
            }
          } else if (fds->message_type()->full_name() ==
                     ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp value =
                dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds);
            if (fds->is_repeated()) {
              dst.GetReflection()->AddMessage(&dst, fds)->CopyFrom(value);
            } else {
              dst.GetReflection()->MutableMessage(&dst, fds)->CopyFrom(value);
            }
          }

          break;
        }

        if (val.IsMap()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg;
          if (fds->is_repeated()) {
            submsg = dst.GetReflection()->AddMessage(&dst, fds);
          } else {
            submsg = dst.GetReflection()->MutableMessage(&dst, fds);
          }
          if (NULL != submsg) {
            dump_message_item(val, *submsg);
          }
        }

        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
        double value = dump_pick_field_with_extensions<double>(val, fds);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddDouble(&dst, fds, value);
        } else {
          dst.GetReflection()->SetDouble(&dst, fds, value);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
        float value = dump_pick_field_with_extensions<float>(val, fds);
        if (fds->is_repeated()) {
          dst.GetReflection()->AddFloat(&dst, fds, value);
        } else {
          dst.GetReflection()->SetFloat(&dst, fds, value);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
        if (!val.IsScalar()) {
          break;
        }

        bool jval = true;
        std::string trans;
        if (val.Scalar().empty()) {
          if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
            trans = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          }
        } else {
          trans = val.Scalar();
        }
        std::transform(trans.begin(), trans.end(), trans.begin(), util::string::tolower<char>);

        if ("0" == trans || "false" == trans || "no" == trans || "disable" == trans || "disabled" == trans ||
            "" == trans) {
          jval = false;
        }

        if (fds->is_repeated()) {
          dst.GetReflection()->AddBool(&dst, fds, jval);
        } else {
          dst.GetReflection()->SetBool(&dst, fds, jval);
        }
        break;
      };
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
        if (!val.IsScalar()) {
          break;
        }

        const std::string *value = &val.Scalar();
        if (NULL == value || value->empty()) {
          if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
            const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
            value = &default_str;
          }
        }

        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *jval = NULL;
        if (NULL == value || value->empty() || ((*value)[0] >= '0' && (*value)[0] <= '9')) {
          jval = fds->enum_type()->FindValueByNumber(util::string::to_int<int32_t>(val.Scalar().c_str()));
        } else {
          jval = fds->enum_type()->FindValueByName(*value);
        }

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
        FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                   fds->type_name());
        break;
      }
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif
}

static void dump_field_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (NULL == fds) {
    return;
  }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    const YAML::Node child = src[fds->name()];
    if (!child) {
      return;
    }
    if (child.IsNull()) {
      return;
    }

    if (fds->is_repeated()) {
      // If it's not sequence, accept one element
      if (child.IsSequence()) {
        for (YAML::Node::const_iterator child_iter = child.begin(); child_iter != child.end(); ++child_iter) {
          dump_pick_field(*child_iter, dst, fds);
        }
      } else {
        dump_pick_field(child, dst, fds);
      }
    } else {
      if (child.IsSequence()) {
        return;
      }
      dump_pick_field(child, dst, fds);
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif
}

static void dump_message_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (NULL == desc) {
    return;
  }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif

    if (!src) {
      return;
    }
    if (!src.IsMap()) {
      return;
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif
  for (int i = 0; i < desc->field_count(); ++i) {
    detail::dump_field_item(src, dst, desc->field(i));
  }
}
}  // namespace detail

LIBATAPP_MACRO_API void parse_timepoint(const std::string &in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out) {
  detail::pick_const_data(in, out);
}

LIBATAPP_MACRO_API void parse_duration(const std::string &in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out) {
  detail::pick_const_data(in, out);
}

LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (NULL == desc) {
    return;
  }

  for (int i = 0; i < desc->field_count(); ++i) {
    detail::dump_field_item(src, dst, desc->field(i));
  }
}

LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                           std::string prefix) {
  if (src.size() > 0) {
    if (1 == src.size()) {
      dst[prefix] = src.as_cpp_string();
    } else {
      for (size_t i = 0; i < src.size(); ++i) {
        std::string sub_prefix =
            prefix.empty() ? LOG_WRAPPER_FWAPI_FORMAT("{}", i) : LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, i);
        dst[sub_prefix] = src.as_cpp_string(i);
      }
    }
  }

  for (util::config::ini_value::node_type::const_iterator iter = src.get_children().begin();
       iter != src.get_children().end(); ++iter) {
    if (!iter->second) {
      continue;
    }
    // First level skip fields already in ::atapp::protocol::atapp_log_sink
    if (prefix.empty()) {
      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds =
          ::atapp::protocol::atapp_log_sink::descriptor()->FindFieldByName(iter->first);
      if (NULL != fds && fds->name() != "unresolved_key_values") {
        continue;
      }
    }

    std::string sub_prefix = prefix.empty() ? iter->first : LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, iter->first);
    ini_loader_dump_to(*iter->second, dst, sub_prefix);
  }
}

LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  detail::dump_message_item(src, dst);
}

LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node &src,
                                            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                            std::string prefix) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    if (!src) {
      return;
    }

    if (src.IsScalar()) {
      dst[prefix] = src.Scalar();
    } else if (src.IsMap()) {
      for (YAML::Node::const_iterator iter = src.begin(); iter != src.end(); ++iter) {
        if (!iter->first || !iter->first.IsScalar()) {
          continue;
        }

        std::string sub_prefix =
            prefix.empty() ? iter->first.Scalar() : LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, iter->first.Scalar());
        yaml_loader_dump_to(iter->second, dst, sub_prefix);
      }
    } else if (src.IsSequence()) {
      if (1 == src.size()) {
        yaml_loader_dump_to(src[0], dst, prefix);
      } else {
        for (size_t i = 0; i < src.size(); ++i) {
          std::string sub_prefix =
              prefix.empty() ? LOG_WRAPPER_FWAPI_FORMAT("{}", i) : LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, i);
          yaml_loader_dump_to(src[i], dst, sub_prefix);
        }
      }
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif
}

LIBATAPP_MACRO_API const YAML::Node yaml_loader_get_child_by_path(const YAML::Node &src, const std::string &path) {
  if (path.empty()) {
    return src;
  }

  if (!src) {
    return src;
  }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif

    YAML::Node ret = src;
    const char *begin = path.c_str();
    const char *end = begin + path.size();

    for (begin = detail::skip_space(begin); end > begin && *begin; begin = detail::skip_space(begin)) {
      const char *old_begin = begin;
      ++begin;
      while (*begin && end > begin && '.' != *begin && ' ' != *begin && '\t' != *begin && '\r' != *begin &&
             '\n' != *begin) {
        ++begin;
      }

      std::string key;
      key.assign(old_begin, begin);

      if (*begin) {
        begin = detail::skip_space(begin);
        if ('.' == *begin) {
          begin = detail::skip_space(begin + 1);
        }
      }

      if (key.empty()) {
        continue;
      }

      if (key[0] >= '0' && key[0] <= '9') {
        size_t idx = util::string::to_int<size_t>(key.c_str());
        if (ret.IsSequence() && ret.size() > idx) {
          YAML::Node child = ret[idx];
          if (child) {
            ret.reset(child);
          } else {
            ret.reset(YAML::Node(YAML::NodeType::Undefined));
          }
        } else {
          ret.reset(YAML::Node(YAML::NodeType::Undefined));
        }
      } else if (ret.IsMap()) {
        YAML::Node child = ret[key];
        if (child) {
          ret.reset(child);
        } else {
          ret.reset(YAML::Node(YAML::NodeType::Undefined));
        }
      } else {
        ret = YAML::Node(YAML::NodeType::Undefined);
      }

      if (!ret) {
        return ret;
      }
    }

    return ret;
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif

  return YAML::Node(YAML::NodeType::Undefined);
}

static bool protobuf_equal_inner_message(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                         const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r);
static bool protobuf_equal_inner_field(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds);

bool protobuf_equal_inner_message(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r) {
  if (&l == &r) {
    return true;
  }

  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = l.GetDescriptor();
  if (desc != r.GetDescriptor()) {
    return false;
  }

  for (int i = 0; i < desc->field_count(); ++i) {
    if (false == protobuf_equal_inner_field(l, r, desc->field(i))) {
      return false;
    }
  }

  return true;
}

bool protobuf_equal_inner_field(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (nullptr == fds) {
    return true;
  }

  int field_size = 0;
  if (fds->is_repeated()) {
    field_size = l.GetReflection()->FieldSize(l, fds);
    if (field_size != r.GetReflection()->FieldSize(r, fds)) {
      return false;
    }
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedInt32(l, fds, i) != r.GetReflection()->GetRepeatedInt32(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetInt32(l, fds) == r.GetReflection()->GetInt32(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedInt64(l, fds, i) != r.GetReflection()->GetRepeatedInt64(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetInt64(l, fds) == r.GetReflection()->GetInt64(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedUInt32(l, fds, i) != r.GetReflection()->GetRepeatedUInt32(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetUInt32(l, fds) == r.GetReflection()->GetUInt32(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedUInt64(l, fds, i) != r.GetReflection()->GetRepeatedUInt64(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetUInt64(l, fds) == r.GetReflection()->GetUInt64(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedString(l, fds, i) != r.GetReflection()->GetRepeatedString(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetString(l, fds) == r.GetReflection()->GetString(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (false == protobuf_equal_inner_message(l.GetReflection()->GetRepeatedMessage(l, fds, i),
                                                    r.GetReflection()->GetRepeatedMessage(r, fds, i))) {
            return false;
          }
        }
      } else {
        return protobuf_equal_inner_message(l.GetReflection()->GetMessage(l, fds),
                                            r.GetReflection()->GetMessage(r, fds));
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedDouble(l, fds, i) != r.GetReflection()->GetRepeatedDouble(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetDouble(l, fds) == r.GetReflection()->GetDouble(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedFloat(l, fds, i) != r.GetReflection()->GetRepeatedFloat(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetFloat(l, fds) == r.GetReflection()->GetFloat(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedBool(l, fds, i) != r.GetReflection()->GetRepeatedBool(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetBool(l, fds) == r.GetReflection()->GetBool(r, fds);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          if (l.GetReflection()->GetRepeatedEnumValue(l, fds, i) !=
              r.GetReflection()->GetRepeatedEnumValue(r, fds, i)) {
            return false;
          }
        }
      } else {
        return l.GetReflection()->GetEnumValue(l, fds) == r.GetReflection()->GetEnumValue(r, fds);
      }
      break;
    };
    default: {
      break;
    }
  }

  return true;
}

LIBATAPP_MACRO_API bool protobuf_equal(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r) {
  return protobuf_equal_inner_message(l, r);
}
}  // namespace atapp