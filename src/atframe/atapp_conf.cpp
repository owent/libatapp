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
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#if defined(GetMessage)
#  undef GetMessage
#endif

#if defined(max)
#  undef max
#endif

#if defined(min)
#  undef min
#endif

LIBATAPP_MACRO_NAMESPACE_BEGIN
namespace {
static const char *skip_space(const char *begin, const char *end) {
  while (nullptr != begin && begin < end && *begin) {
    if (::atfw::util::string::is_space(*begin)) {
      ++begin;
      continue;
    }
    break;
  }

  return begin;
}

static void dynamic_copy_protobuf_duration_or_timestamp(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *duration_ptr,
                                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &value) {
  // Database may be different and so descriptor may also be different
  // We can't use CopyFrom here
  if (duration_ptr->GetDescriptor() == ::google::protobuf::Duration::descriptor()) {
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

static void dynamic_copy_protobuf_duration_or_timestamp(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *timestamp_ptr,
                                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &value) {
  // Database may be different and so descriptor may also be different
  // We can't use CopyFrom here
  if (timestamp_ptr->GetDescriptor() == ::google::protobuf::Timestamp::descriptor()) {
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

  if (begin + 1 < end && '0' == begin[0] && 'x' == atfw::util::string::tolower(begin[1])) {  // hex
    begin += 2;
    while (begin < end && begin && *begin) {
      char c = atfw::util::string::tolower(*begin);
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
  if (nullptr != word_begin && nullptr != word_end && word_end > word_begin) {
    unit.assign(word_begin, word_end);
    std::transform(unit.begin(), unit.end(), unit.begin(), ::atfw::util::string::tolower<char>);
  }

  bool fallback = true;
  do {
    if (unit.empty() || unit == "s" || unit == "sec" || unit == "second" || unit == "seconds") {
      break;
    }

    if (unit == "ms" || unit == "millisecond" || unit == "milliseconds") {
      fallback = false;
      dur.set_seconds(tm_val / 1000);
      dur.set_nanos(static_cast<int32_t>((tm_val % 1000) * 1000000));
      break;
    }

    if (unit == "us" || unit == "microsecond" || unit == "microseconds") {
      fallback = false;
      dur.set_seconds(tm_val / 1000000);
      dur.set_nanos(static_cast<int32_t>((tm_val % 1000000) * 1000));
      break;
    }

    if (unit == "ns" || unit == "nanosecond" || unit == "nanoseconds") {
      fallback = false;
      dur.set_seconds(tm_val / 1000000000);
      dur.set_nanos(static_cast<int32_t>(tm_val % 1000000000));
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

  struct tm t;  // NOLINT(cppcoreguidelines-pro-type-member-init)
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
    res -= ::atfw::util::time::time_utility::get_sys_zone_offset();
  } else if (word_begin < word_end && *word_begin == '+') {
    res -= ::atfw::util::time::time_utility::get_sys_zone_offset();
    time_t offset = 0;
    word_begin = pick_number(offset, word_begin + 1, word_end);
    res -= offset * 60;
    if (word_begin < word_end && *word_begin && ':' == *word_begin) {
      pick_number(offset, word_begin + 1, word_end);
      res -= offset;
    }
    timepoint.set_seconds(timepoint.seconds() - offset);
  } else if (word_begin < word_end && *word_begin == '-') {
    res -= ::atfw::util::time::time_utility::get_sys_zone_offset();
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

struct ATFW_UTIL_SYMBOL_LOCAL enum_alias_mapping_t {
  std::unordered_map<std::string, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *> origin;
  std::unordered_map<std::string, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *> no_case;
};

static const enum_alias_mapping_t *get_enum_value_alias_mapping(
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumDescriptor *desc) {
  if (desc == nullptr) {
    return nullptr;
  }

  static std::mutex g_enum_alias_mutex;
  std::lock_guard<std::mutex> lg(g_enum_alias_mutex);
  static std::unordered_map<std::string, atfw::util::memory::strong_rc_ptr<enum_alias_mapping_t>> g_enum_alias_mappings;

  std::string full_name = std::string{desc->full_name()};
  auto it = g_enum_alias_mappings.find(full_name);
  if (it != g_enum_alias_mappings.end()) {
    return it->second.get();
  }

  atfw::util::memory::strong_rc_ptr<enum_alias_mapping_t> res =
      atfw::util::memory::make_strong_rc<enum_alias_mapping_t>();
  g_enum_alias_mappings[full_name] = res;

  for (int i = 0; i < desc->value_count(); ++i) {
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *value_desc = desc->value(i);
    if (value_desc == nullptr) {
      continue;
    }

    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueOptions &options = value_desc->options();
    if (!options.HasExtension(protocol::ENUMVALUE)) {
      continue;
    }

    const protocol::atapp_configure_enumvalue_options &ext = options.GetExtension(protocol::ENUMVALUE);
    for (int j = 0; j < ext.alias_name_size(); ++j) {
      if (ext.alias_name(j).empty()) {
        continue;
      }

      res->origin[std::string{ext.alias_name(j)}] = value_desc;
      if (!ext.case_sensitive()) {
        std::string lower_alias = std::string{ext.alias_name(j)};
        std::transform(lower_alias.begin(), lower_alias.end(), lower_alias.begin(),
                       ::atfw::util::string::tolower<char>);
        res->no_case[lower_alias] = value_desc;
      }
    }
  }

  return res.get();
}

static const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *pick_enum_value_from_alias(
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumDescriptor *desc, gsl::string_view input) {
  const enum_alias_mapping_t *mapping = get_enum_value_alias_mapping(desc);
  if (mapping == nullptr) {
    return nullptr;
  }

  auto it = mapping->origin.find(static_cast<std::string>(input));
  if (it != mapping->origin.end()) {
    return it->second;
  }

  if (!mapping->no_case.empty()) {
    std::string lower_input = static_cast<std::string>(input);
    std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::atfw::util::string::tolower<char>);
    it = mapping->no_case.find(lower_input);

    if (it != mapping->no_case.end()) {
      return it->second;
    }
  }

  return nullptr;
}

static inline void dump_pick_field_min(bool &out) { out = false; }

static inline void dump_pick_field_min(std::string &) {}

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

static inline void dump_pick_field_max(bool &out) { out = true; }

static inline void dump_pick_field_max(std::string &out) { out = ""; }

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

static inline void dump_pick_field_from_str(bool &out, gsl::string_view input, bool) {
  out = dump_pick_logic_bool(input);
}

static inline void dump_pick_field_from_str(std::string &out, gsl::string_view input, bool) {
  out = static_cast<std::string>(input);
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
  char *end = nullptr;
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
  char *end = nullptr;
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
  const char *left = atfw::util::string::str2int(out, input);

  if (size_mode) {
    while (left && *left && atfw::util::string::is_space(*left)) {
      ++left;
    }

    if (left) {
      scale_size_mode_t<Ty, sizeof(Ty)>::to_bytes(out, atfw::util::string::tolower(*left));
    }
  }
}

static inline bool dump_pick_field_less(const bool &l, const bool &r) {
  if (l == r) {
    return false;
  }

  return r;
}

static inline bool dump_pick_field_less(const std::string &, const std::string &) { return false; }

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

static std::pair<const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *, bool>
dump_pick_enum_field_with_extensions(gsl::string_view val_str,
                                     const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  bool is_default = false;
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *ret = nullptr;
  if (nullptr == fds || nullptr == fds->enum_type()) {
    return std::pair<const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *, bool>(ret, is_default);
  }

  if (val_str.empty()) {
    if (fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
      const atapp::protocol::atapp_configure_meta &meta = fds->options().GetExtension(atapp::protocol::CONFIGURE);
      val_str = meta.default_value();
    }

    is_default = true;
  }

  if (!val_str.empty()) {
    if (val_str[0] >= '0' && val_str[0] <= '9') {
      ret = fds->enum_type()->FindValueByNumber(atfw::util::string::to_int<int32_t>(val_str));
    } else {
      ret = fds->enum_type()->FindValueByName(static_cast<std::string>(val_str));
    }
    if (ret == nullptr) {
      ret = pick_enum_value_from_alias(fds->enum_type(), val_str);
    }
  }

  if (ret == nullptr) {
    ret = fds->enum_type()->FindValueByNumber(0);
    is_default = true;
  }

  return {ret, is_default};
}

template <class ValueType>
static bool dump_field_with_value_delegate(
    const std::pair<ValueType, bool> &value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds, configure_key_set *dump_existed_set,
    gsl::string_view existed_set_prefix,
    void (google::protobuf::Reflection::*add_fn)(google::protobuf::Message *, const google::protobuf::FieldDescriptor *,
                                                 ValueType value) const,
    void (google::protobuf::Reflection::*set_fn)(google::protobuf::Message *, const google::protobuf::FieldDescriptor *,
                                                 ValueType value) const) {
  if (fds->is_repeated()) {
    if (!value.second) {
      if (nullptr != dump_existed_set) {
        dump_existed_set->insert(atfw::util::log::format("{}{}.{}", existed_set_prefix, fds->name(),
                                                         dst.GetReflection()->FieldSize(dst, fds)));
      }
      (dst.GetReflection()->*add_fn)(&dst, fds, value.first);
      return true;
    }
    return false;
  }

  std::string existed_field_key = atfw::util::log::format("{}{}", existed_set_prefix, fds->name());
  if (value.second) {
    if (nullptr == dump_existed_set || dump_existed_set->end() == dump_existed_set->find(existed_field_key)) {
      (dst.GetReflection()->*set_fn)(&dst, fds, value.first);
      return true;
    }
  } else {
    (dst.GetReflection()->*set_fn)(&dst, fds, value.first);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(existed_field_key);
    }
    return true;
  }
  return false;
}

template <class MessageType>
static bool dump_field_with_message_delegate(const std::pair<MessageType, bool> &value,
                                             ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                             const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                             configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (fds->is_repeated()) {
    if (!value.second) {
      if (nullptr != dump_existed_set) {
        dump_existed_set->insert(atfw::util::log::format("{}{}.{}", existed_set_prefix, fds->name(),
                                                         dst.GetReflection()->FieldSize(dst, fds)));
      }
      ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *msg_ptr = dst.GetReflection()->AddMessage(&dst, fds);
      if (nullptr != msg_ptr) {
        dynamic_copy_protobuf_duration_or_timestamp(msg_ptr, value.first);
        return true;
      }
    }
    return false;
  }

  std::string existed_field_key = atfw::util::log::format("{}{}", existed_set_prefix, fds->name());
  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *msg_ptr = nullptr;
  if (value.second) {
    if (nullptr == dump_existed_set || dump_existed_set->end() == dump_existed_set->find(existed_field_key)) {
      msg_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
    }
  } else {
    msg_ptr = dst.GetReflection()->MutableMessage(&dst, fds);
    if (nullptr != dump_existed_set) {
      dump_existed_set->insert(existed_field_key);
    }
  }

  if (nullptr != msg_ptr) {
    dynamic_copy_protobuf_duration_or_timestamp(msg_ptr, value.first);
  }
  return nullptr != msg_ptr;
}

static bool dump_field_with_value(const std::pair<int32_t, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddInt32,
                                        &google::protobuf::Reflection::SetInt32);
}

static bool dump_field_with_value(const std::pair<uint32_t, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddUInt32,
                                        &google::protobuf::Reflection::SetUInt32);
}

static bool dump_field_with_value(const std::pair<int64_t, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddInt64,
                                        &google::protobuf::Reflection::SetInt64);
}

static bool dump_field_with_value(const std::pair<uint64_t, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddUInt64,
                                        &google::protobuf::Reflection::SetUInt64);
}

static bool dump_field_with_value(const std::pair<std::string, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddString,
                                        &google::protobuf::Reflection::SetString);
}

static bool dump_field_with_value(const std::pair<bool, bool> &value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddBool, &google::protobuf::Reflection::SetBool);
}

static bool dump_field_with_value(const std::pair<float, bool> &value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddFloat,
                                        &google::protobuf::Reflection::AddFloat);
}

static bool dump_field_with_value(const std::pair<double, bool> &value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddDouble,
                                        &google::protobuf::Reflection::SetDouble);
}

static bool dump_field_with_value(
    const std::pair<const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor *, bool> &value,
    ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
    configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_value_delegate(value, dst, fds, dump_existed_set, existed_set_prefix,
                                        &google::protobuf::Reflection::AddEnum, &google::protobuf::Reflection::SetEnum);
}

static bool dump_field_with_value(const std::pair<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_message_delegate(value, dst, fds, dump_existed_set, existed_set_prefix);
}

static bool dump_field_with_value(const std::pair<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration, bool> &value,
                                  ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                  configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  return dump_field_with_message_delegate(value, dst, fds, dump_existed_set, existed_set_prefix);
}

template <class TRET>
static std::pair<TRET, bool> dump_pick_field_with_extensions(
    gsl::string_view val_str, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  TRET value, min_value, max_value;
  bool is_default = false;
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

    is_default = true;
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

  return std::pair<TRET, bool>(value, is_default);
}

template <class TRET>
static inline std::pair<TRET, bool> dump_pick_field_with_extensions(
    const atfw::util::config::ini_value &val, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
    size_t index) {
  return dump_pick_field_with_extensions<TRET>(val.as_cpp_string(index), fds);
}

static void dump_pick_default_field(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                    const configure_key_set &existed_set, gsl::string_view prefix) {
  if (nullptr == fds) {
    return;
  }

  // If it's oneof and other fields are set, skip
  if (nullptr != fds->containing_oneof()) {
    if (dst.GetReflection()->HasOneof(dst, fds->containing_oneof())) {
      return;
    }
  }

  bool allow_string_default_value = nullptr == fds->message_type();
  if (!allow_string_default_value) {
    allow_string_default_value =
        fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name() ||
        fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name();
  }

  if (allow_string_default_value) {
    if (!fds->options().HasExtension(atapp::protocol::CONFIGURE)) {
      return;
    }

    if (fds->options().GetExtension(atapp::protocol::CONFIGURE).default_value().empty()) {
      return;
    }

    if (fds->is_repeated()) {
      FWLOGERROR("{} in {} is a repeated field, we do not support to set default value now", fds->name(),
                 dst.GetDescriptor()->full_name());
      return;
    }

    // Already has value, skip. Only check leaf values
    if (existed_set.end() != existed_set.find(atfw::util::log::format("{}{}", prefix, fds->name()))) {
      return;
    }
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      dst.GetReflection()->SetInt32(&dst, fds, dump_pick_field_with_extensions<int32_t>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      dst.GetReflection()->SetInt64(&dst, fds, dump_pick_field_with_extensions<int64_t>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      dst.GetReflection()->SetUInt32(&dst, fds, dump_pick_field_with_extensions<uint32_t>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      dst.GetReflection()->SetUInt64(&dst, fds, dump_pick_field_with_extensions<uint64_t>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      dst.GetReflection()->SetString(&dst, fds, dump_pick_field_with_extensions<std::string>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      dst.GetReflection()->SetEnum(&dst, fds, dump_pick_enum_field_with_extensions("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      dst.GetReflection()->SetBool(&dst, fds, dump_pick_field_with_extensions<bool>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      // Recursive scan repeated messages
      if (fds->is_repeated()) {
        int list_size = dst.GetReflection()->FieldSize(dst, fds);
        for (int i = 0; i < list_size; ++i) {
          auto *submsg = dst.GetReflection()->MutableRepeatedMessage(&dst, fds, i);
          std::string new_prefix = atfw::util::log::format("{}{}.{}.", prefix, fds->name(), i);
          for (int j = 0; submsg != nullptr && j < submsg->GetDescriptor()->field_count(); ++j) {
            dump_pick_default_field(*submsg, submsg->GetDescriptor()->field(j), existed_set, new_prefix);
          }
        }
      } else {
        // special message
        google::protobuf::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (fds->message_type()->full_name() ==
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration value =
              dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>("", fds).first;
          dynamic_copy_protobuf_duration_or_timestamp(submsg, value);
        } else if (fds->message_type()->full_name() ==
                   ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp value =
              dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>("", fds).first;
          dynamic_copy_protobuf_duration_or_timestamp(submsg, value);
        } else {
          std::string new_prefix = atfw::util::log::format("{}{}.", prefix, fds->name());
          for (int i = 0; submsg != nullptr && i < submsg->GetDescriptor()->field_count(); ++i) {
            dump_pick_default_field(*submsg, submsg->GetDescriptor()->field(i), existed_set, new_prefix);
          }
        }
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      dst.GetReflection()->SetFloat(&dst, fds, dump_pick_field_with_extensions<float>("", fds).first);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      dst.GetReflection()->SetDouble(&dst, fds, dump_pick_field_with_extensions<double>("", fds).first);
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

static bool dump_pick_map_key_field(gsl::string_view val_str, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                    configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (nullptr == fds) {
    return false;
  }

  if (fds->is_repeated()) {
    return false;
  }

  bool ret = false;
  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      auto value = dump_pick_field_with_extensions<int32_t>(val_str, fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      auto value = dump_pick_field_with_extensions<int64_t>(val_str, fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      auto value = dump_pick_field_with_extensions<uint32_t>(val_str, fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      auto value = dump_pick_field_with_extensions<uint64_t>(val_str, fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      auto value = dump_pick_field_with_extensions<std::string>(val_str, fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      auto value = dump_pick_field_with_extensions<bool>(val_str, fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
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

  return ret;
}

static bool dump_pick_field(const atfw::util::config::ini_value &val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds, size_t index,
                            configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  bool ret = false;
  if (nullptr == fds) {
    return ret;
  }

  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      auto value = dump_pick_field_with_extensions<int32_t>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      auto value = dump_pick_field_with_extensions<int64_t>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      auto value = dump_pick_field_with_extensions<uint32_t>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      auto value = dump_pick_field_with_extensions<uint64_t>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      auto value = dump_pick_field_with_extensions<std::string>(val.as_cpp_string(index), fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      // special message
      if (fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
        auto value = dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>(val, fds, index);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      } else if (fds->message_type()->full_name() ==
                 ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
        auto value = dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds, index);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
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

        atfw::util::config::ini_value::node_type::const_iterator iter = val.get_children().begin();

        for (; iter != val.get_children().end(); ++iter) {
          if (!iter->second) {
            continue;
          }

          int idx = dst.GetReflection()->FieldSize(dst, fds);
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
          if (nullptr == submsg) {
            break;
          }
          std::string submsg_map_existed_set_key =
              atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), idx);

          bool has_data = false;
          if (dump_pick_map_key_field(iter->first, *submsg, key_fds, dump_existed_set, submsg_map_existed_set_key)) {
            has_data = true;
          }
          if (dump_pick_field(*iter->second, *submsg, value_fds, 0, dump_existed_set, submsg_map_existed_set_key)) {
            has_data = true;
          }

          if (has_data) {
            ret = true;
            if (dump_existed_set != nullptr) {
              dump_existed_set->insert(atfw::util::log::format("{}{}.{}", existed_set_prefix, fds->name(), idx));
            }
          }
        }
      } else if (fds->is_repeated()) {
        // repeated message is unpack by PARENT.0.field = XXX
        for (uint32_t j = 0;; ++j) {
          std::string idx = atfw::util::log::format("{}", j);
          atfw::util::config::ini_value::node_type::const_iterator idx_iter = val.get_children().find(idx);
          if (idx_iter == val.get_children().end() || !idx_iter->second) {
            break;
          }

          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->AddMessage(&dst, fds);
          if (nullptr != submsg) {
            if (ini_loader_dump_to(*idx_iter->second, *submsg, dump_existed_set,
                                   atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), j))) {
              ret = true;
              if (dump_existed_set != nullptr) {
                dump_existed_set->insert(atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), j));
              }
            }
          }
        }
        break;
      } else {
        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (nullptr != submsg) {
          if (ini_loader_dump_to(val, *submsg, dump_existed_set,
                                 atfw::util::log::format("{}{}.", existed_set_prefix, fds->name()))) {
            ret = true;
            if (dump_existed_set != nullptr) {
              dump_existed_set->insert(atfw::util::log::format("{}{}", existed_set_prefix, fds->name()));
            }
          }
        }
      }

      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      auto value = dump_pick_field_with_extensions<double>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      auto value = dump_pick_field_with_extensions<float>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      auto value = dump_pick_field_with_extensions<bool>(val, fds, index);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      auto value = dump_pick_enum_field_with_extensions(val.as_cpp_string(index), fds);
      ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
      break;
    }
    default: {
      FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                 fds->type_name());
      break;
    }
  }

  return ret;
}

static bool dump_field_item(const atfw::util::config::ini_value &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                            configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (nullptr == fds) {
    return false;
  }

  // 同层级展开
  if (!fds->is_repeated() && fds->options().HasExtension(atfw::atapp::protocol::CONFIGURE)) {
    auto &opt_ext = fds->options().GetExtension(atfw::atapp::protocol::CONFIGURE);
    auto &field_match = opt_ext.field_match();
    if (!field_match.field_name().empty() && !field_match.field_value().empty()) {
      atfw::util::config::ini_value::node_type::const_iterator field_value_iter =
          src.get_children().find(field_match.field_name());
      if (field_value_iter == src.get_children().end()) {
        return false;
      }
      if (!field_value_iter->second) {
        return false;
      }
      if (field_value_iter->second->as_cpp_string() != field_match.field_value()) {
        return false;
      }

      ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
      if (nullptr == submsg) {
        return false;
      }
      return ini_loader_dump_to(src, *submsg, dump_existed_set, existed_set_prefix);
    }
  }

  atfw::util::config::ini_value::node_type::const_iterator child_iter =
      src.get_children().find(static_cast<std::string>(fds->name()));
  // skip if not found, just skip
  if (child_iter == src.get_children().end()) {
    return false;
  }

  if (!child_iter->second) {
    return false;
  }

  if (fds->is_repeated() && fds->cpp_type() != ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE) {
    size_t arrsz = child_iter->second->size();
    int ret = false;
    for (size_t i = 0; i < arrsz; ++i) {
      if (dump_pick_field(*child_iter->second, dst, fds, i, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
    }
    return ret;
  } else {
    return dump_pick_field(*child_iter->second, dst, fds, 0, dump_existed_set, existed_set_prefix);
  }
}

template <class TRET>
static std::pair<TRET, bool> dump_pick_field_with_extensions(
    const YAML::Node &val, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds) {
  bool is_default = false;
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

  return std::pair<TRET, bool>(value, is_default);
}

static bool dump_message_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                              configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix);

static bool dump_pick_field(const YAML::Node &val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                            configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (nullptr == fds) {
    return false;
  }

  bool ret = false;

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif

    if (!val) {
      return false;
    }
    if (val.IsNull() || val.IsSequence()) {
      return false;
    }

    switch (fds->cpp_type()) {
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
        auto value = dump_pick_field_with_extensions<int32_t>(val, fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
        auto value = dump_pick_field_with_extensions<int64_t>(val, fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
        auto value = dump_pick_field_with_extensions<uint32_t>(val, fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
        auto value = dump_pick_field_with_extensions<uint64_t>(val, fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
        if (!val.IsScalar()) {
          break;
        }

        auto value = dump_pick_field_with_extensions<std::string>(val.Scalar(), fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
        // special message
        if (val.IsScalar()) {
          if (fds->message_type()->full_name() ==
              ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
            auto value = dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>(val, fds);
            ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
          } else if (fds->message_type()->full_name() ==
                     ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
            auto value = dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds);
            ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
          }

          break;
        }

        if (val.IsMap()) {
          ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = nullptr;
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
              int index = dst.GetReflection()->FieldSize(dst, fds);
              submsg = dst.GetReflection()->AddMessage(&dst, fds);
              if (nullptr == submsg) {
                break;
              }

              std::string submsg_map_existed_set_key =
                  atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), index);
              bool has_data = false;
              if (dump_pick_field(iter->first, *submsg, key_fds, dump_existed_set, submsg_map_existed_set_key)) {
                has_data = true;
              }
              if (dump_pick_field(iter->second, *submsg, value_fds, dump_existed_set, submsg_map_existed_set_key)) {
                has_data = true;
              }

              if (has_data) {
                ret = true;
                if (dump_existed_set != nullptr) {
                  dump_existed_set->insert(atfw::util::log::format("{}{}.{}", existed_set_prefix, fds->name(), index));
                }
              }
            }
          } else if (fds->is_repeated()) {
            int index = dst.GetReflection()->FieldSize(dst, fds);
            submsg = dst.GetReflection()->AddMessage(&dst, fds);
            if (nullptr == submsg) {
              break;
            }
            if (dump_message_item(val, *submsg, dump_existed_set,
                                  atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), index))) {
              ret = true;
              if (dump_existed_set != nullptr) {
                dump_existed_set->insert(atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), index));
              }
            }
          } else {
            submsg = dst.GetReflection()->MutableMessage(&dst, fds);
            if (nullptr == submsg) {
              break;
            }

            if (dump_message_item(val, *submsg, dump_existed_set,
                                  atfw::util::log::format("{}{}.", existed_set_prefix, fds->name()))) {
              ret = true;
              if (dump_existed_set != nullptr) {
                dump_existed_set->insert(atfw::util::log::format("{}{}", existed_set_prefix, fds->name()));
              }
            }
          }
        }

        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
        auto value = dump_pick_field_with_extensions<double>(val, fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
        auto value = dump_pick_field_with_extensions<float>(val, fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
        if (!val.IsScalar()) {
          break;
        }

        auto value = dump_pick_field_with_extensions<bool>(val.Scalar(), fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
        if (!val.IsScalar()) {
          break;
        }

        auto value = dump_pick_enum_field_with_extensions(val.Scalar(), fds);
        ret = dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix);
        break;
      }
      default: {
        FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(),
                   fds->type_name());
        break;
      }
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Ignore error
  }
#endif

  return ret;
}

static bool dump_field_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                            configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (nullptr == fds) {
    return false;
  }

  int ret = false;
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif

    // 同层级展开
    if (!fds->is_repeated() && fds->options().HasExtension(atfw::atapp::protocol::CONFIGURE)) {
      auto &opt_ext = fds->options().GetExtension(atfw::atapp::protocol::CONFIGURE);
      auto &field_match = opt_ext.field_match();
      if (!field_match.field_name().empty() && !field_match.field_value().empty()) {
        const YAML::Node field_value_node = src[field_match.field_name()];
        if (!field_value_node.IsScalar()) {
          return false;
        }
        if (field_value_node.Scalar() != field_match.field_value()) {
          return false;
        }

        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (nullptr == submsg) {
          return false;
        }
        return dump_message_item(src, *submsg, dump_existed_set, existed_set_prefix);
      }
    }

    const YAML::Node child = src[fds->name()];
    if (!child) {
      return false;
    }
    if (child.IsNull()) {
      return false;
    }

    if (fds->is_repeated()) {
      // If it's not sequence, accept one element

      if (child.IsSequence()) {
        for (YAML::Node::const_iterator child_iter = child.begin(); child_iter != child.end(); ++child_iter) {
          if (dump_pick_field(*child_iter, dst, fds, dump_existed_set, existed_set_prefix)) {
            ret = true;
          }
        }
      } else {
        ret = dump_pick_field(child, dst, fds, dump_existed_set, existed_set_prefix);
      }
    } else {
      if (child.IsSequence()) {
        return false;
      }
      ret = dump_pick_field(child, dst, fds, dump_existed_set, existed_set_prefix);
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Ignore error
  }
#endif
  return ret;
}

static bool dump_environment_message_item(gsl::string_view prefix, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                          configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix);

static bool dump_environment_pick_field(const std::string &key, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                        configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (nullptr == fds) {
    return false;
  }

  bool ret = false;
  switch (fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<int32_t>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<int64_t>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<uint32_t>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<uint64_t>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<std::string>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      // special message
      if (!val.empty()) {
        if (fds->message_type()->full_name() ==
            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
          auto value = dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration>(val, fds);
          if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
            ret = true;
          }
        } else if (fds->message_type()->full_name() ==
                   ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
          auto value = dump_pick_field_with_extensions<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp>(val, fds);
          if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
            ret = true;
          }
        }

        break;
      }

      ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg;
      if (fds->is_repeated()) {
        int index = dst.GetReflection()->FieldSize(dst, fds);
        submsg = dst.GetReflection()->AddMessage(&dst, fds);
        if (nullptr == submsg) {
          break;
        }
        if (dump_environment_message_item(
                key, *submsg, dump_existed_set,
                atfw::util::log::format("{}{}.{}.", existed_set_prefix, fds->name(), index))) {
          ret = true;
          if (dump_existed_set != nullptr) {
            dump_existed_set->insert(atfw::util::log::format("{}{}.{}", existed_set_prefix, fds->name(), index));
          }
        } else {
          dst.GetReflection()->RemoveLast(&dst, fds);
        }
      } else {
        submsg = dst.GetReflection()->MutableMessage(&dst, fds);
        if (nullptr == submsg) {
          break;
        }
        if (dump_environment_message_item(key, *submsg, dump_existed_set,
                                          atfw::util::log::format("{}{}.", existed_set_prefix, fds->name()))) {
          ret = true;
          if (nullptr != dump_existed_set) {
            dump_existed_set->insert(atfw::util::log::format("{}{}", existed_set_prefix, fds->name()));
          }
        }
      }

      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<double>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }
      auto value = dump_pick_field_with_extensions<float>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }

      auto value = dump_pick_field_with_extensions<bool>(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_ENUM: {
      std::string val = atfw::util::file_system::getenv(key.c_str());
      if (val.empty()) {
        break;
      }

      auto value = dump_pick_enum_field_with_extensions(val, fds);
      if (dump_field_with_value(value, dst, fds, dump_existed_set, existed_set_prefix)) {
        ret = true;
      }
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
                                        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds,
                                        configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  if (nullptr == fds) {
    return false;
  }

  // 同层级展开
  if (!fds->is_repeated() && fds->options().HasExtension(atfw::atapp::protocol::CONFIGURE)) {
    auto &opt_ext = fds->options().GetExtension(atfw::atapp::protocol::CONFIGURE);
    auto &field_match = opt_ext.field_match();
    if (!field_match.field_name().empty() && !field_match.field_value().empty()) {
      std::string env_key_prefix;
      env_key_prefix.reserve(prefix.size() + 1 + fds->name().size());
      if (!prefix.empty()) {
        env_key_prefix = static_cast<std::string>(prefix);
        env_key_prefix += "_";
      }

      env_key_prefix += field_match.field_name();
      std::transform(env_key_prefix.begin(), env_key_prefix.end(), env_key_prefix.begin(),
                     atfw::util::string::toupper<char>);
      std::string field_value_env = atfw::util::file_system::getenv(env_key_prefix.c_str());
      if (field_value_env != field_match.field_value()) {
        return false;
      }

      ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *submsg = dst.GetReflection()->MutableMessage(&dst, fds);
      if (nullptr == submsg) {
        return false;
      }
      return dump_environment_message_item(std::string(prefix), *submsg, dump_existed_set, existed_set_prefix);
    }
  }

  std::string env_key_prefix;
  env_key_prefix.reserve(prefix.size() + 1 + fds->name().size());
  if (!prefix.empty()) {
    env_key_prefix = static_cast<std::string>(prefix);
    env_key_prefix += "_";
  }

  env_key_prefix += fds->name();
  std::transform(env_key_prefix.begin(), env_key_prefix.end(), env_key_prefix.begin(),
                 atfw::util::string::toupper<char>);

  if (fds->is_repeated()) {
    bool has_value = false;
    for (size_t i = 0;; ++i) {
      if (dump_environment_pick_field(atfw::util::log::format("{}_{}", env_key_prefix, i), dst, fds, dump_existed_set,
                                      existed_set_prefix)) {
        has_value = true;
      } else {
        break;
      }
    }
    // Fallback to no-index key
    if (!has_value) {
      return dump_environment_pick_field(env_key_prefix, dst, fds, dump_existed_set, existed_set_prefix);
    }
    return true;
  } else {
    return dump_environment_pick_field(env_key_prefix, dst, fds, dump_existed_set, existed_set_prefix);
  }
}

static bool dump_message_item(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                              configure_key_set *dump_existed_set, gsl::string_view prefix) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
    return false;
  }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif

    if (!src) {
      return false;
    }
    if (!src.IsMap()) {
      return false;
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Ignore error
  }
#endif
  bool ret = false;
  for (int i = 0; i < desc->field_count(); ++i) {
    if (dump_field_item(src, dst, desc->field(i), dump_existed_set, prefix)) {
      ret = true;
    }
  }

  return ret;
}

static bool dump_environment_message_item(gsl::string_view prefix, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                          configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
    return false;
  }

  bool ret = false;
  for (int i = 0; i < desc->field_count(); ++i) {
    bool res = dump_environment_field_item(prefix, dst, desc->field(i), dump_existed_set, existed_set_prefix);
    ret = ret || res;
  }

  return ret;
}

}  // namespace

LIBATAPP_MACRO_API void parse_timepoint(gsl::string_view in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp &out) {
  pick_const_data(in, out);
}

LIBATAPP_MACRO_API void parse_duration(gsl::string_view in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration &out) {
  pick_const_data(in, out);
}

LIBATAPP_MACRO_API bool ini_loader_dump_to(const atfw::util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                           configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor *desc = dst.GetDescriptor();
  if (nullptr == desc) {
    return false;
  }

  bool ret = false;
  for (int i = 0; i < desc->field_count(); ++i) {
    if (dump_field_item(src, dst, desc->field(i), dump_existed_set, existed_set_prefix)) {
      ret = true;
    }
  }

  return ret;
}

LIBATAPP_MACRO_API bool ini_loader_dump_to(const atfw::util::config::ini_value &src,
                                           ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                           gsl::string_view prefix, configure_key_set *dump_existed_set,
                                           gsl::string_view existed_set_prefix) {
  bool ret = false;
  if (src.size() > 0) {
    if (1 == src.size()) {
      dst[static_cast<std::string>(prefix)] = src.as_cpp_string();
      ret = true;

      if (nullptr != dump_existed_set) {
        if (!existed_set_prefix.empty()) {
          dump_existed_set->insert(
              static_cast<std::string>(existed_set_prefix.substr(0, existed_set_prefix.size() - 1)));
        } else {
          dump_existed_set->insert(static_cast<std::string>(existed_set_prefix));
        }
      }
    } else {
      for (size_t i = 0; i < src.size(); ++i) {
        std::string sub_prefix =
            prefix.empty() ? atfw::util::log::format("{}", i) : atfw::util::log::format("{}.{}", prefix, i);
        dst[sub_prefix] = src.as_cpp_string(i);
        ret = true;

        if (nullptr != dump_existed_set) {
          dump_existed_set->insert(atfw::util::log::format("{}{}", existed_set_prefix, i));
        }
      }
    }
  }

  for (atfw::util::config::ini_value::node_type::const_iterator iter = src.get_children().begin();
       iter != src.get_children().end(); ++iter) {
    if (!iter->second) {
      continue;
    }
    // First level skip fields already in ::atframework::atapp::protocol::atapp_log_sink
    if (prefix.empty()) {
      const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds =
          ::atframework::atapp::protocol::atapp_log_sink::descriptor()->FindFieldByName(iter->first);
      if (nullptr != fds && fds->name() != "unresolved_key_values") {
        continue;
      }
    }

    std::string sub_prefix = prefix.empty() ? iter->first : atfw::util::log::format("{}.{}", prefix, iter->first);
    if (ini_loader_dump_to(*iter->second, dst, sub_prefix, dump_existed_set,
                           atfw::util::log::format("{}{}.", existed_set_prefix, iter->first))) {
      ret = true;
    }
  }

  return ret;
}

LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node &src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                            configure_key_set *dump_existed_set, gsl::string_view existed_set_prefix) {
  dump_message_item(src, dst, dump_existed_set, existed_set_prefix);
}

LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node &src,
                                            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string> &dst,
                                            gsl::string_view prefix, configure_key_set *dump_existed_set,
                                            gsl::string_view existed_set_prefix) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  try {
#endif
    if (!src) {
      return;
    }

    if (src.IsScalar()) {
      dst[static_cast<std::string>(prefix)] = src.Scalar();
      if (nullptr != dump_existed_set) {
        if (!existed_set_prefix.empty()) {
          dump_existed_set->insert(
              static_cast<std::string>(existed_set_prefix.substr(0, existed_set_prefix.size() - 1)));
        } else {
          dump_existed_set->insert(static_cast<std::string>(existed_set_prefix));
        }
      }
    } else if (src.IsMap()) {
      for (YAML::Node::const_iterator iter = src.begin(); iter != src.end(); ++iter) {
        if (!iter->first || !iter->first.IsScalar()) {
          continue;
        }

        std::string sub_prefix =
            prefix.empty() ? iter->first.Scalar() : atfw::util::log::format("{}.{}", prefix, iter->first.Scalar());
        yaml_loader_dump_to(iter->second, dst, sub_prefix, dump_existed_set,
                            atfw::util::log::format("{}{}.", existed_set_prefix, iter->first.Scalar()));
      }
    } else if (src.IsSequence()) {
      if (1 == src.size()) {
        yaml_loader_dump_to(src[0], dst, prefix, dump_existed_set,
                            atfw::util::log::format("{}{}.", existed_set_prefix, 0));
      } else {
        for (size_t i = 0; i < src.size(); ++i) {
          std::string sub_prefix =
              prefix.empty() ? atfw::util::log::format("{}", i) : atfw::util::log::format("{}.{}", prefix, i);
          yaml_loader_dump_to(src[i], dst, sub_prefix, dump_existed_set,
                              atfw::util::log::format("{}{}.", existed_set_prefix, i));
        }
      }
    }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
  } catch (...) {  // NOLINT(bugprone-empty-catch)
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

  for (begin = skip_space(begin, end); end > begin && *begin; begin = skip_space(begin, end)) {
    const char *old_begin = begin;
    ++begin;
    while (*begin && end > begin && '.' != *begin && ' ' != *begin && '\t' != *begin && '\r' != *begin &&
           '\n' != *begin) {
      ++begin;
    }

    std::string key;
    key.assign(old_begin, begin);

    if (*begin) {
      begin = skip_space(begin, end);
      if ('.' == *begin) {
        begin = skip_space(begin + 1, end);
      }
    }

    if (key.empty()) {
      continue;
    }

    keys_storage.emplace_back(std::move(key));
  }

  keys_span.reserve(keys_storage.size());
  for (auto &key : keys_storage) {
    keys_span.emplace_back(key);
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
        size_t idx = atfw::util::string::to_int<size_t>(key.c_str());
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
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Ignore error
  }
#endif

  return ret;
}

LIBATAPP_MACRO_API bool environment_loader_dump_to(gsl::string_view prefix,
                                                   ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                                   configure_key_set *dump_existed_set,
                                                   gsl::string_view existed_set_prefix) {
  return dump_environment_message_item(prefix, dst, dump_existed_set, existed_set_prefix);
}

LIBATAPP_MACRO_API void default_loader_dump_to(ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &dst,
                                               const configure_key_set &existed_set) {
  for (int i = 0; i < dst.GetDescriptor()->field_count(); ++i) {
    auto *fds = dst.GetDescriptor()->field(i);
    if (fds == nullptr) {
      continue;
    }

    dump_pick_default_field(dst, dst.GetDescriptor()->field(i), existed_set, "");
  }
}

namespace {
static bool protobuf_equal_inner_message(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                         const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r);
static bool protobuf_equal_inner_field(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *fds);

static bool protobuf_equal_inner_map_bool(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                          const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                          const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
                                          const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
                                          const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  auto lreflect = l.GetReflection();
  auto rreflect = r.GetReflection();
  int field_size = lreflect->FieldSize(l, field_fds);
  std::unordered_map<bool, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *> lmap;
  lmap.reserve(static_cast<size_t>(field_size));
  for (int i = 0; i < field_size; ++i) {
    auto lvalue = &lreflect->GetRepeatedMessage(l, field_fds, i);
    auto lkey = lvalue->GetReflection()->GetRepeatedBool(*lvalue, key_fds, i);
    lmap[lkey] = lvalue;
  }

  for (int i = 0; i < field_size; ++i) {
    auto rvalue = &rreflect->GetRepeatedMessage(r, field_fds, i);
    auto rkey = rvalue->GetReflection()->GetRepeatedBool(*rvalue, key_fds, i);
    auto iter = lmap.find(rkey);
    if (iter == lmap.end()) {
      return false;
    }

    // Only compare value
    if (false == protobuf_equal_inner_field(*iter->second, *rvalue, value_fds)) {
      return false;
    }
  }

  return true;
}

static bool protobuf_equal_inner_map_int32(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  auto lreflect = l.GetReflection();
  auto rreflect = r.GetReflection();
  int field_size = lreflect->FieldSize(l, field_fds);
  std::unordered_map<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::int32, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *> lmap;
  lmap.reserve(static_cast<size_t>(field_size));
  for (int i = 0; i < field_size; ++i) {
    auto lvalue = &lreflect->GetRepeatedMessage(l, field_fds, i);
    auto lkey = lvalue->GetReflection()->GetRepeatedInt32(*lvalue, key_fds, i);
    lmap[lkey] = lvalue;
  }

  for (int i = 0; i < field_size; ++i) {
    auto rvalue = &rreflect->GetRepeatedMessage(r, field_fds, i);
    auto rkey = rvalue->GetReflection()->GetRepeatedInt32(*rvalue, key_fds, i);
    auto iter = lmap.find(rkey);
    if (iter == lmap.end()) {
      return false;
    }

    // Only compare value
    if (false == protobuf_equal_inner_field(*iter->second, *rvalue, value_fds)) {
      return false;
    }
  }

  return true;
}

static bool protobuf_equal_inner_map_uint32(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  auto lreflect = l.GetReflection();
  auto rreflect = r.GetReflection();
  int field_size = lreflect->FieldSize(l, field_fds);
  std::unordered_map<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::uint32, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *>
      lmap;
  lmap.reserve(static_cast<size_t>(field_size));
  for (int i = 0; i < field_size; ++i) {
    auto lvalue = &lreflect->GetRepeatedMessage(l, field_fds, i);
    auto lkey = lvalue->GetReflection()->GetRepeatedUInt32(*lvalue, key_fds, i);
    lmap[lkey] = lvalue;
  }

  for (int i = 0; i < field_size; ++i) {
    auto rvalue = &rreflect->GetRepeatedMessage(r, field_fds, i);
    auto rkey = rvalue->GetReflection()->GetRepeatedUInt32(*rvalue, key_fds, i);
    auto iter = lmap.find(rkey);
    if (iter == lmap.end()) {
      return false;
    }

    // Only compare value
    if (false == protobuf_equal_inner_field(*iter->second, *rvalue, value_fds)) {
      return false;
    }
  }

  return true;
}

static bool protobuf_equal_inner_map_int64(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
                                           const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  auto lreflect = l.GetReflection();
  auto rreflect = r.GetReflection();
  int field_size = lreflect->FieldSize(l, field_fds);
  std::unordered_map<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::int64, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *> lmap;
  lmap.reserve(static_cast<size_t>(field_size));
  for (int i = 0; i < field_size; ++i) {
    auto lvalue = &lreflect->GetRepeatedMessage(l, field_fds, i);
    auto lkey = lvalue->GetReflection()->GetRepeatedInt64(*lvalue, key_fds, i);
    lmap[lkey] = lvalue;
  }

  for (int i = 0; i < field_size; ++i) {
    auto rvalue = &rreflect->GetRepeatedMessage(r, field_fds, i);
    auto rkey = rvalue->GetReflection()->GetRepeatedInt64(*rvalue, key_fds, i);
    auto iter = lmap.find(rkey);
    if (iter == lmap.end()) {
      return false;
    }

    // Only compare value
    if (false == protobuf_equal_inner_field(*iter->second, *rvalue, value_fds)) {
      return false;
    }
  }

  return true;
}

static bool protobuf_equal_inner_map_uint64(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  auto lreflect = l.GetReflection();
  auto rreflect = r.GetReflection();
  int field_size = lreflect->FieldSize(l, field_fds);
  std::unordered_map<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::uint64, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *>
      lmap;
  lmap.reserve(static_cast<size_t>(field_size));
  for (int i = 0; i < field_size; ++i) {
    auto lvalue = &lreflect->GetRepeatedMessage(l, field_fds, i);
    auto lkey = lvalue->GetReflection()->GetRepeatedUInt64(*lvalue, key_fds, i);
    lmap[lkey] = lvalue;
  }

  for (int i = 0; i < field_size; ++i) {
    auto rvalue = &rreflect->GetRepeatedMessage(r, field_fds, i);
    auto rkey = rvalue->GetReflection()->GetRepeatedUInt64(*rvalue, key_fds, i);
    auto iter = lmap.find(rkey);
    if (iter == lmap.end()) {
      return false;
    }

    // Only compare value
    if (false == protobuf_equal_inner_field(*iter->second, *rvalue, value_fds)) {
      return false;
    }
  }

  return true;
}

static bool protobuf_equal_inner_map_string(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
                                            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  auto lreflect = l.GetReflection();
  auto rreflect = r.GetReflection();
  int field_size = lreflect->FieldSize(l, field_fds);
  std::unordered_map<std::string, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message *> lmap;
  lmap.reserve(static_cast<size_t>(field_size));
  for (int i = 0; i < field_size; ++i) {
    auto lvalue = &lreflect->GetRepeatedMessage(l, field_fds, i);
    auto lkey = lvalue->GetReflection()->GetRepeatedString(*lvalue, key_fds, i);
    lmap[std::move(lkey)] = lvalue;
  }

  for (int i = 0; i < field_size; ++i) {
    auto rvalue = &rreflect->GetRepeatedMessage(r, field_fds, i);
    auto rkey = rvalue->GetReflection()->GetRepeatedString(*rvalue, key_fds, i);
    auto iter = lmap.find(rkey);
    if (iter == lmap.end()) {
      return false;
    }

    // Only compare value
    if (false == protobuf_equal_inner_field(*iter->second, *rvalue, value_fds)) {
      return false;
    }
  }

  return true;
}

static std::pair<bool, bool> protobuf_equal_inner_map(
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r,
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *field_fds,
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds,
    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds) {
  switch (field_fds->cpp_type()) {
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL:
      return {true, protobuf_equal_inner_map_bool(l, r, field_fds, key_fds, value_fds)};
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32:
      return {true, protobuf_equal_inner_map_int32(l, r, field_fds, key_fds, value_fds)};
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64:
      return {true, protobuf_equal_inner_map_int64(l, r, field_fds, key_fds, value_fds)};
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32:
      return {true, protobuf_equal_inner_map_uint32(l, r, field_fds, key_fds, value_fds)};
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64:
      return {true, protobuf_equal_inner_map_uint64(l, r, field_fds, key_fds, value_fds)};
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING:
      return {true, protobuf_equal_inner_map_string(l, r, field_fds, key_fds, value_fds)};
    default:
      break;
  }

  return {false, false};
}

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
        do {
          if (!fds->is_map()) {
            break;
          }
          const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *key_fds = fds->message_type()->map_key();
          const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor *value_fds = fds->message_type()->map_value();
          if (nullptr == key_fds || nullptr == value_fds) {
            break;
          }

          auto res = protobuf_equal_inner_map(l, r, fds, key_fds, value_fds);
          if (!res.first) {
            break;
          }
          return res.second;
        } while (false);

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
          auto lv = l.GetReflection()->GetRepeatedDouble(l, fds, i);
          auto rv = r.GetReflection()->GetRepeatedDouble(r, fds, i);
          if (std::abs(lv - rv) > std::numeric_limits<double>::epsilon() * std::abs(lv + rv) * 2 &&
              std::abs(lv - rv) >= std::numeric_limits<double>::min()) {
            return false;
          }
        }
      } else {
        auto lv = l.GetReflection()->GetDouble(l, fds);
        auto rv = r.GetReflection()->GetDouble(r, fds);

        return std::abs(lv - rv) <= std::numeric_limits<double>::epsilon() * std::abs(lv + rv) * 2 ||
               std::abs(lv - rv) < std::numeric_limits<double>::min();
      }
      break;
    };
    case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
      if (fds->is_repeated()) {
        for (int i = 0; i < field_size; ++i) {
          auto lv = l.GetReflection()->GetRepeatedFloat(l, fds, i);
          auto rv = r.GetReflection()->GetRepeatedFloat(r, fds, i);
          if (std::abs(lv - rv) > std::numeric_limits<float>::epsilon() * std::abs(lv + rv) * 2 &&
              std::abs(lv - rv) >= std::numeric_limits<float>::min()) {
            return false;
          }
        }
      } else {
        auto lv = l.GetReflection()->GetFloat(l, fds);
        auto rv = r.GetReflection()->GetFloat(r, fds);
        return std::abs(lv - rv) <= std::numeric_limits<float>::epsilon() * std::abs(lv + rv) * 2 ||
               std::abs(lv - rv) < std::numeric_limits<float>::min();
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
}  // namespace

LIBATAPP_MACRO_API bool protobuf_equal(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &l,
                                       const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message &r) {
  return protobuf_equal_inner_message(l, r);
}
LIBATAPP_MACRO_NAMESPACE_END
