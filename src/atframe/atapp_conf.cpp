// Copyright 2021 atframework
// Created by owent

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include <config/compiler/protobuf_prefix.h>

#include <yaml-cpp/yaml.h>

#include <google/protobuf/duration.pb.h>
#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>
#include <google/protobuf/timestamp.pb.h>

#include <config/compiler/protobuf_suffix.h>

// Must include protobuf first, or MinGW will redefine GetMessage -> GetMessageA
#include <atframe/atapp_conf.h>

#include <libatbus.h>

#include <common/file_system.h>
#include <common/string_oprs.h>
#include <config/atframe_utils_build_feature.h>
#include <config/ini_loader.h>
#include <log/log_wrapper.h>
#include <time/time_utility.h>

#include <algorithm>
#include <limits>
#include <sstream>

#if defined(GetMessage)
#  undef GetMessage
#endif

#if defined(max)
#  undef max
#endif

#if defined(min)
#  undef min
#endif

namespace atapp {
namespace detail {
static const char *skip_space(const char *begin, const char *end) {
  while (begin && begin < end && *begin) {
    if (::util::string::is_space(*begin)) {
      ++begin;
      continue;
    }
    break;
  }

  return begin;
}

static void dynamic_copy_protobuf_duration(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *duration_ptr,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &value) {
  // Database may be different and so descriptor may also be different
  // We can't use CopyFrom here
  if (duration_ptr->GetDescriptor() == value.GetDescriptor()) {
    duration_ptr->CopyFrom(value);
  } else {
    duration_ptr->GetReflection()->SetInt64(duration_ptr,
                                            duration_ptr->GetDescriptor()->FindFieldByNumber(
                                                ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::kSecondsFieldNumber),
                                            value.seconds());
    duration_ptr->GetReflection()->SetInt32(duration_ptr,
                                            duration_ptr->GetDescriptor()->FindFieldByNumber(
                                                ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::kNanosFieldNumber),
                                            value.nanos());
  }
}

static void dynamic_copy_protobuf_timestamp(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *timestamp_ptr,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &value) {
  // Database may be different and so descriptor may also be different
  // We can't use CopyFrom here
  if (timestamp_ptr->GetDescriptor() == value.GetDescriptor()) {
    timestamp_ptr->CopyFrom(value);
  } else {
    timestamp_ptr->GetReflection()->SetInt64(timestamp_ptr,
                                             timestamp_ptr->GetDescriptor()->FindFieldByNumber(
                                                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::kSecondsFieldNumber),
                                             value.seconds());
    timestamp_ptr->GetReflection()->SetInt32(timestamp_ptr,
                                             timestamp_ptr->GetDescriptor()->FindFieldByNumber(
                                                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::kNanosFieldNumber),
                                             value.nanos());
  }
}

template <typename TINT>
static const char *pick_number(TINT &out, const char *begin, const char *end) {
  out = 0;
  if (nullptr == begin || begin >= end || !(*begin)) {
    return begin;
  }

  // negative
  bool is_negative = false;
  while (begin < end && *begin && *begin == '-') {
    is_negative = !is_negative;
    ++begin;
  }

  if (begin >= end || !(*begin)) {
    return begin;
  }

  if (begin + 1 < end && '0' == begin[0] && 'x' == util::string::tolower(begin[1])) {  // hex
    begin += 2;
    while (begin < end && begin && *begin) {
      char c = util::string::tolower(*begin);
      if (c >= '0' && c <= '9') {
        out <<= 4;
        out += c - '0';
        ++begin;
      } else if (c >= 'a' && c <= 'f') {
        out <<= 4;
        out += c - 'a' + 10;
        ++begin;
      } else {
        break;
      }
    }
  } else if ('\\' == begin[0]) {
    ++begin;
    while (begin < end && begin && *begin >= '0' && *begin <= '7') {
      out <<= 3;
      out += *begin - '0';
      ++begin;
    }
  } else {
    while (begin < end && begin && *begin >= '0' && *begin <= '9') {
      out *= 10;
      out += *begin - '0';
      ++begin;
    }
  }

  if (is_negative) {
    out = (~out) + 1;
  }

  return begin;
}

/*
static bool protobuf_field_cmp_fn(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* l, const
ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* r) { int lv = (nullptr == l)? 0: l->number(); int rv = (nullptr ==
r)? 0: r->number(); return lv < rv;
}
*/

static void pick_const_data(gsl::string_view value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &dur) {
  dur.set_seconds(0);
  dur.set_nanos(0);

  int64_t tm_val = 0;
  const char *word_begin = value.data();
  const char *word_end = word_begin + value.size();
  word_begin = skip_space(word_begin, word_end);
  word_begin = pick_number(tm_val, word_begin, word_end);
  word_begin = skip_space(word_begin, word_end);

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

static void pick_const_data(gsl::string_view value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &timepoint) {
  timepoint.set_seconds(0);
  timepoint.set_nanos(0);

  const char *word_begin = value.data();
  const char *word_end = word_begin + value.size();
  word_begin = skip_space(word_begin, word_end);

  struct tm t;
  memset(&t, 0, sizeof(t));

  // year
  {
    word_begin = pick_number(t.tm_year, word_begin, word_end);
    word_begin = skip_space(word_begin, word_end);
    if (word_begin < word_end && *word_begin == '-') {
      ++word_begin;
      word_begin = skip_space(word_begin, word_end);
    }
    t.tm_year -= 1900;  // years since 1900
  }
  // month
  {
    word_begin = pick_number(t.tm_mon, word_begin, word_end);
    word_begin = skip_space(word_begin, word_end);
    if (word_begin < word_end && *word_begin == '-') {
      ++word_begin;
      word_begin = skip_space(word_begin, word_end);
    }

    --t.tm_mon;  // [0, 11]
  }
  // day
  {
    word_begin = pick_number(t.tm_mday, word_begin, word_end);
    word_begin = skip_space(word_begin, word_end);
    if (word_begin < word_end && *word_begin == 'T') {  // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
      ++word_begin;
      word_begin = skip_space(word_begin, word_end);
    }
  }

  // tm_hour
  {
    word_begin = pick_number(t.tm_hour, word_begin, word_end);
    word_begin = skip_space(word_begin, word_end);
    if (word_begin < word_end && *word_begin == ':') {  // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
      ++word_begin;
      word_begin = skip_space(word_begin, word_end);
    }
  }

  // tm_min
  {
    word_begin = pick_number(t.tm_min, word_begin, word_end);
    word_begin = skip_space(word_begin, word_end);
    if (word_begin < word_end && *word_begin == ':') {  // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
      ++word_begin;
      word_begin = skip_space(word_begin, word_end);
    }
  }

  // tm_sec
  {
    word_begin = pick_number(t.tm_sec, word_begin, word_end);
    word_begin = skip_space(word_begin, word_end);
  }

  time_t res = mktime(&t);

  if (word_begin < word_end && *word_begin == 'Z') {  // UTC timezone
    res -= ::util::time::time_utility::get_sys_zone_offset();
  } else if (word_begin < word_end && *word_begin == '+') {
    res -= ::util::time::time_utility::get_sys_zone_offset();
    time_t offset = 0;
    word_begin = pick_number(offset, word_begin + 1, word_end);
    res -= offset * 60;
    if (word_begin < word_end && *word_begin && ':' == *word_begin) {
      pick_number(offset, word_begin + 1, word_end);
      res -= offset;
    }
    timepoint.set_seconds(timepoint.seconds() - offset);
  } else if (word_begin < word_end && *word_begin == '-') {
    res -= ::util::time::time_utility::get_sys_zone_offset();
    time_t offset = 0;
    word_begin = pick_number(offset, word_begin + 1, word_end);
    res += offset * 60;
    if (word_begin < word_end && *word_begin && ':' == *word_begin) {
      pick_number(offset, word_begin + 1, word_end);
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

static inline void dump_pick_field_from_str(float &out, gsl::string_view input, bool) {
  if (input.empty()) {
    out = 0.0f;
    return;
  }
  char input_string[32] = {0};
  if (input.size() < sizeof(input_string) - 1) {
    memcpy(input_string, input.data(), input.size());
  } else {
    memcpy(input_string, input.data(), sizeof(input_string) - 1);
  }
  char *end;
  out = static_cast<float>(strtod(input_string, &end));
}

static inline void dump_pick_field_from_str(double &out, gsl::string_view input, bool) {
  if (input.empty()) {
    out = 0.0;
    return;
  }
  char input_string[32] = {0};
  if (input.size() < sizeof(input_string) - 1) {
    memcpy(input_string, input.data(), input.size());
  } else {
    memcpy(input_string, input.data(), sizeof(input_string) - 1);
  }
  char *end;
  out = strtod(input_string, &end);
}

static inline void dump_pick_field_from_str(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out, gsl::string_view input,
                                            bool) {
  if (input.empty()) {
    out.set_seconds(0);
    out.set_nanos(0);
    return;
  }
  pick_const_data(input, out);
}
static inline void dump_pick_field_from_str(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out, gsl::string_view input,
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
  static inline bool to_bytes(Ty &, char) { return false; }
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
static inline void dump_pick_field_from_str(Ty &out, gsl::string_view input, bool size_mode) {
  if (input.empty()) {
    out = 0;
    return;
  }
  const char *left = util::string::str2int(out, input);

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
static TRET dump_pick_field_with_extensions(gsl::string_view val_str,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  TRET value, min_value, max_value;
  dump_pick_field_min(min_value);
  dump_pick_field_max(max_value);
  if (val_str.empty()) {
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
      dump_pick_field_from_str(value, val_str, meta.size_mode());
    } else {
      dump_pick_field_from_str(value, val_str, false);
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

template <class TRET>
static inline TRET dump_pick_field_with_extensions(const util::config::ini_value &val,
                                                   const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                                   size_t index) {
  return dump_pick_field_with_extensions<TRET>(val.as_cpp_string(index), fds);
}

static inline bool dump_pick_logic_bool(gsl::string_view trans) {
  if (trans.empty()) {
    return false;
  }

  if (0 == UTIL_STRFUNC_STRNCASE_CMP("0", trans.data(), trans.size())) {
    return false;
  }

  if (0 == UTIL_STRFUNC_STRNCASE_CMP("false", trans.data(), trans.size())) {
    return false;
  }

  if (0 == UTIL_STRFUNC_STRNCASE_CMP("no", trans.data(), trans.size())) {
    return false;
  }

  if (0 == UTIL_STRFUNC_STRNCASE_CMP("disable", trans.data(), trans.size())) {
    return false;
  }

  if (0 == UTIL_STRFUNC_STRNCASE_CMP("disabled", trans.data(), trans.size())) {
    return false;
  }

  return true;
}

static void dump_pick_map_key_field(gsl::string_view val_str, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (nullptr == fds) {
    return;
  }

  if (fds->is_repeated()) {
    return;
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      int32_t value = dump_pick_field_with_extensions<int32_t>(val_str, fds);
      dst.GetReflection()->SetInt32(&dst, fds, value);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      int64_t value = dump_pick_field_with_extensions<int64_t>(val_str, fds);
      dst.GetReflection()->SetInt64(&dst, fds, value);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      uint32_t value = dump_pick_field_with_extensions<uint32_t>(val_str, fds);
      dst.GetReflection()->SetUInt32(&dst, fds, value);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      uint64_t value = dump_pick_field_with_extensions<uint64_t>(val_str, fds);
      dst.GetReflection()->SetUInt64(&dst, fds, value);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      dst.GetReflection()->SetString(&dst, fds, static_cast<std::string>(val_str));
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      bool jval = dump_pick_logic_bool(val_str);

      dst.GetReflection()->SetBool(&dst, fds, jval);
      break;
    };
    default: {
      // See https://developers.google.com/protocol-buffers/docs/proto3#maps
      // key_type can be any integral or string type (so, any scalar type except for floating point types and bytes).
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                 fds->type_name());
      break;
    }
  }
}

static void dump_pick_field(const util::config::ini_value &val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds, size_t index) {
  if (nullptr == fds) {
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
      if (nullptr == value || value->empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          value = &default_str;
        }
      }
      if (nullptr != value) {
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
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *duration_ptr;
        if (fds->is_repeated()) {
          duration_ptr = dst.GetReflection()->AddMessage(&dst, fds);
        } else {
          duration_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
        }
        detail::dynamic_copy_protobuf_duration(duration_ptr, value);
        break;
      } else if (fds->message_type()->full_name() ==
                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp value =
            dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds, index);
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *timestamp_ptr;
        if (fds->is_repeated()) {
          timestamp_ptr = dst.GetReflection()->AddMessage(&dst, fds);
        } else {
          timestamp_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
        }
        detail::dynamic_copy_protobuf_timestamp(timestamp_ptr, value);
        break;
      }

      if (fds->is_map()) {
        if (nullptr == fds->message_type()) {
          break;
        }
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds = fds->message_type()->map_key();
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds = fds->message_type()->map_value();
        if (nullptr == key_fds || nullptr == value_fds) {
          break;
        }

        util::config::ini_value::node_type::const_iterator iter = val.get_children().begin();
        for (; iter != val.get_children().end(); ++iter) {
          if (!iter->second) {
            continue;
          }

          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
          if (nullptr == submsg) {
            break;
          }
          dump_pick_map_key_field(iter->first, *submsg, key_fds);
          dump_pick_field(*iter->second, *submsg, value_fds, 0);
        }
      } else if (fds->is_repeated()) {
        // repeated message is unpack by PARENT.0.field = XXX
        for (uint32_t j = 0;; ++j) {
          std::string idx = LOG_WRAPPER_FWAPI_FORMAT("{}", j);
          util::config::ini_value::node_type::const_iterator idx_iter = val.get_children().find(idx);
          if (idx_iter == val.get_children().end() || !idx_iter->second) {
            break;
          }

          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
          if (nullptr != submsg) {
            ini_loader_dump_to(*idx_iter->second, *submsg);
          }
        }
        break;
      } else {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (nullptr != submsg) {
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
      std::string trans;
      if (val.as_cpp_string(index).empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          trans = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
        }
      } else {
        trans = val.as_cpp_string(index);
      }
      bool jval = dump_pick_logic_bool(trans);

      if (fds->is_repeated()) {
        dst.GetReflection()->AddBool(&dst, fds, jval);
      } else {
        dst.GetReflection()->SetBool(&dst, fds, jval);
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      const std::string *value = &val.as_cpp_string(index);
      if (nullptr == value || value->empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          value = &default_str;
        }
      }

      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *jval = nullptr;
      if (nullptr == value || value->empty() || ((*value)[0] >= '0' && (*value)[0] <= '9')) {
        jval = fds->enum_type()->FindValueByNumber(val.as_int32(index));
      } else {
        jval = fds->enum_type()->FindValueByName(*value);
      }

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

static void dump_field_item(const util::config::ini_value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (nullptr == fds) {
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
  if (nullptr == fds) {
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
        if (nullptr == value || value->empty()) {
          if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
            const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
            value = &default_str;
          }
        }
        if (nullptr != value) {
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
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *duration_ptr;
            if (fds->is_repeated()) {
              duration_ptr = dst.GetReflection()->AddMessage(&dst, fds);
            } else {
              duration_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
            }
            detail::dynamic_copy_protobuf_duration(duration_ptr, value);
          } else if (fds->message_type()->full_name() ==
                     ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp value =
                dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds);
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *timestamp_ptr;
            if (fds->is_repeated()) {
              timestamp_ptr = dst.GetReflection()->AddMessage(&dst, fds);
            } else {
              timestamp_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
            }
            detail::dynamic_copy_protobuf_timestamp(timestamp_ptr, value);
          }

          break;
        }

        if (val.IsMap()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg;
          if (fds->is_map()) {
            if (nullptr == fds->message_type()) {
              break;
            }
            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds = fds->message_type()->map_key();
            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds = fds->message_type()->map_value();
            if (nullptr == key_fds || nullptr == value_fds) {
              break;
            }

            YAML::Node::const_iterator iter = val.begin();
            for (; iter != val.end(); ++iter) {
              submsg = dst.GetReflection()->AddMessage(&dst, fds);
              if (nullptr == submsg) {
                break;
              }
              dump_pick_field(iter->first, *submsg, key_fds);
              dump_pick_field(iter->second, *submsg, value_fds);
            }
          } else if (fds->is_repeated()) {
            submsg = dst.GetReflection()->AddMessage(&dst, fds);
            if (nullptr == submsg) {
              break;
            }
            dump_message_item(val, *submsg);
          } else {
            submsg = dst.GetReflection()->MutableMessage(&dst, fds);
            if (nullptr == submsg) {
              break;
            }
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
        if (nullptr == value || value->empty()) {
          if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
            const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
            value = &default_str;
          }
        }

        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *jval = nullptr;
        if (nullptr == value || value->empty() || ((*value)[0] >= '0' && (*value)[0] <= '9')) {
          jval = fds->enum_type()->FindValueByNumber(util::string::to_int<int32_t>(val.Scalar().c_str()));
        } else {
          jval = fds->enum_type()->FindValueByName(*value);
        }

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

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif
}

static void dump_field_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (nullptr == fds) {
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

static bool dump_environment_message_item(gsl::string_view prefix, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst);
static bool dump_environment_pick_field(const std::string &key, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (nullptr == fds) {
    return false;
  }

  bool ret = false;
  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      int32_t value = dump_pick_field_with_extensions<int32_t>(val, fds);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddInt32(&dst, fds, value);
      } else {
        dst.GetReflection()->SetInt32(&dst, fds, value);
      }
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      int64_t value = dump_pick_field_with_extensions<int64_t>(val, fds);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddInt64(&dst, fds, value);
      } else {
        dst.GetReflection()->SetInt64(&dst, fds, value);
      }
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      uint32_t value = dump_pick_field_with_extensions<uint32_t>(val, fds);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddUInt32(&dst, fds, value);
      } else {
        dst.GetReflection()->SetUInt32(&dst, fds, value);
      }
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      uint64_t value = dump_pick_field_with_extensions<uint64_t>(val, fds);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddUInt64(&dst, fds, value);
      } else {
        dst.GetReflection()->SetUInt64(&dst, fds, value);
      }
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }

      const std::string *value = &val;
      if (nullptr == value || value->empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          value = &default_str;
        }
      }
      if (nullptr != value) {
        if (fds->is_repeated()) {
          dst.GetReflection()->AddString(&dst, fds, val);
        } else {
          dst.GetReflection()->SetString(&dst, fds, val);
        }
      }

      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      std::string val = util::file_system::getenv(key.c_str());
      // special message
      if (!val.empty()) {
        if (fds->message_type()->full_name() ==
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration value =
              dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>(val, fds);
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *duration_ptr;
          if (fds->is_repeated()) {
            duration_ptr = dst.GetReflection()->AddMessage(&dst, fds);
          } else {
            duration_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
          }
          detail::dynamic_copy_protobuf_duration(duration_ptr, value);
          ret = true;
        } else if (fds->message_type()->full_name() ==
                   ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp value =
              dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds);
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *timestamp_ptr;
          if (fds->is_repeated()) {
            timestamp_ptr = dst.GetReflection()->AddMessage(&dst, fds);
          } else {
            timestamp_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
          }
          detail::dynamic_copy_protobuf_timestamp(timestamp_ptr, value);
          ret = true;
        }

        break;
      }

      ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg;
      if (fds->is_repeated()) {
        submsg = dst.GetReflection()->AddMessage(&dst, fds);
        if (nullptr == submsg) {
          break;
        }
        if (dump_environment_message_item(key, *submsg)) {
          ret = true;
        } else {
          dst.GetReflection()->RemoveLast(&dst, fds);
        }
      } else {
        submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (nullptr == submsg) {
          break;
        }
        if (dump_environment_message_item(key, *submsg)) {
          ret = true;
        }
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      double value = dump_pick_field_with_extensions<double>(val, fds);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddDouble(&dst, fds, value);
      } else {
        dst.GetReflection()->SetDouble(&dst, fds, value);
      }
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      float value = dump_pick_field_with_extensions<float>(val, fds);
      if (fds->is_repeated()) {
        dst.GetReflection()->AddFloat(&dst, fds, value);
      } else {
        dst.GetReflection()->SetFloat(&dst, fds, value);
      }
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }

      bool jval = true;
      std::string trans;
      if (val.empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          trans = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
        }
      } else {
        trans = val;
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
      ret = true;
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      std::string val = util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }

      const std::string *value = &val;
      if (nullptr == value || value->empty()) {
        if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
          const std::string &default_str = fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value();
          value = &default_str;
        }
      }

      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *jval = nullptr;
      if (nullptr == value || value->empty() || ((*value)[0] >= '0' && (*value)[0] <= '9')) {
        jval = fds->enum_type()->FindValueByNumber(util::string::to_int<int32_t>(val.c_str()));
      } else {
        jval = fds->enum_type()->FindValueByName(*value);
      }

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
      ret = true;
      break;
    };
    default: {
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                 fds->type_name());
      break;
    }
  }

  return ret;
}

static bool dump_environment_field_item(gsl::string_view prefix, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  if (nullptr == fds) {
    return false;
  }

  std::string env_key_prefix;
  env_key_prefix.reserve(prefix.size() + 1 + fds->name().size());
  if (!prefix.empty()) {
    env_key_prefix = static_cast<std::string>(prefix);
    env_key_prefix += "_";
  }

  env_key_prefix += fds->name();
  std::transform(env_key_prefix.begin(), env_key_prefix.end(), env_key_prefix.begin(), util::string::toupper<char>);

  if (fds->is_repeated()) {
    bool has_value = false;
    for (size_t i = 0;; ++i) {
      if (dump_environment_pick_field(util::log::format("{}_{}", env_key_prefix, i), dst, fds)) {
        has_value = true;
      } else {
        break;
      }
    }
    // Fallback to no-index key
    if (!has_value) {
      return dump_environment_pick_field(env_key_prefix, dst, fds);
    }
    return true;
  } else {
    return dump_environment_pick_field(env_key_prefix, dst, fds);
  }
}

static void dump_message_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
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

static bool dump_environment_message_item(gsl::string_view prefix, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
    return false;
  }

  bool ret = false;
  for (int i = 0; i < desc->field_count(); ++i) {
    bool res = detail::dump_environment_field_item(prefix, dst, desc->field(i));
    ret = ret || res;
  }

  return ret;
}

}  // namespace detail

LIBATAPP_MACRO_API void parse_timepoint(gsl::string_view in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out) {
  detail::pick_const_data(in, out);
}

LIBATAPP_MACRO_API void parse_duration(gsl::string_view in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out) {
  detail::pick_const_data(in, out);
}

LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
    return;
  }

  for (int i = 0; i < desc->field_count(); ++i) {
    detail::dump_field_item(src, dst, desc->field(i));
  }
}

LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                           gsl::string_view prefix) {
  if (src.size() > 0) {
    if (1 == src.size()) {
      dst[static_cast<std::string>(prefix)] = src.as_cpp_string();
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
      if (nullptr != fds && fds->name() != "unresolved_key_values") {
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
                                            gsl::string_view prefix) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    if (!src) {
      return;
    }

    if (src.IsScalar()) {
      dst[static_cast<std::string>(prefix)] = src.Scalar();
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

LIBATAPP_MACRO_API const YAML::Node yaml_loader_get_child_by_path(const YAML::Node &src, gsl::string_view path) {
  if (!src || path.empty()) {
    return src;
  }

  if (!src) {
    return src;
  }

  std::vector<std::string> keys_storage;
  std::vector<gsl::string_view> keys_span;

  const char *begin = path.data();
  const char *end = begin + path.size();

  for (begin = detail::skip_space(begin, end); end > begin && *begin; begin = detail::skip_space(begin, end)) {
    const char *old_begin = begin;
    ++begin;
    while (*begin && end > begin && '.' != *begin && ' ' != *begin && '\t' != *begin && '\r' != *begin &&
           '\n' != *begin) {
      ++begin;
    }

    std::string key;
    key.assign(old_begin, begin);

    if (*begin) {
      begin = detail::skip_space(begin, end);
      if ('.' == *begin) {
        begin = detail::skip_space(begin + 1, end);
      }
    }

    if (key.empty()) {
      continue;
    }

    keys_storage.emplace_back(std::move(key));
  }

  keys_span.reserve(keys_storage.size());
  for (auto &key : keys_storage) {
    keys_span.push_back(key);
  }

  return yaml_loader_get_child_by_path(src, keys_span);
}

LIBATAPP_MACRO_API const YAML::Node yaml_loader_get_child_by_path(const YAML::Node &src,
                                                                  const std::vector<gsl::string_view> &path,
                                                                  size_t start_path_index) {
  if (!src || start_path_index >= path.size()) {
    return src;
  }

  YAML::Node ret = src;
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    for (; start_path_index < path.size(); ++start_path_index) {
      std::string key = static_cast<std::string>(path[start_path_index]);
      if (key.empty()) {
        continue;
      }

      if (ret.IsSequence() && key[0] >= '0' && key[0] <= '9') {
        size_t idx = util::string::to_int<size_t>(key.c_str());
        if (ret.size() > idx) {
          YAML::Node child = ret[idx];
          if (child) {
            ret.reset(child);
          } else {
            break;
          }
        }
      } else if (ret.IsMap()) {
        YAML::Node child = ret[key];
        if (child) {
          ret.reset(child);
        } else {
          break;
        }
      } else {
        break;
      }
    }
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {
    // Ignore error
  }
#endif

  return ret;
}

LIBATAPP_MACRO_API bool environment_loader_dump_to(gsl::string_view prefix,
                                                   ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst) {
  return detail::dump_environment_message_item(prefix, dst);
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
