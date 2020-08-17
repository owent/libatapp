#include <algorithm>
#include <sstream>

#include "libatbus.h"

#include <config/atframe_utils_build_feature.h>
#include <common/string_oprs.h>
#include <time/time_utility.h>
#include <config/ini_loader.h>
#include <log/log_wrapper.h>

#include <config/compiler/protobuf_prefix.h>

#include "yaml-cpp/yaml.h"

#include <google/protobuf/message.h>
#include <google/protobuf/repeated_field.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <config/compiler/protobuf_suffix.h>

#include <atframe/atapp_conf.h>

#ifdef GetMessage
#undef GetMessage
#endif

namespace atapp {
    namespace detail {
        static const char* skip_space(const char *str) {
            while (str && *str) {
                if (::util::string::is_space(*str)) {
                    ++str;
                    continue;
                }
                break;
            }

            return str;
        }

        template<typename TINT>
        static const char* pick_number(TINT &out, const char *str) {
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
            if ('0' == str[0] && 'x' == util::string::tolower(str[1])) { // hex
                str += 2;
                while(str && *str) {
                    char c = util::string::tolower(*str);
                    if (c >= '0' && c <= '9') {
                        out <<= 4;
                        out += c - '0';
                        ++ str;
                    } else if (c >= 'a' && c <= 'f') {
                        out <<= 4;
                        out += c - 'a' + 10;
                        ++ str;
                    } else {
                        break;
                    }
                }
            } else if ('\\' == str[0]) {
                ++ str;
                while(str && *str >= '0' && *str <= '7') {
                    out <<= 3;
                    out += *str - '0';
                    ++ str;
                }
            } else {
                while(str && *str >= '0' && *str <= '9') {
                    out *= 10;
                    out += *str - '0';
                    ++ str;
                }
            }

            if (is_negative) {
                out = (~out) + 1;
            }

            return str;
        }

        /*
        static bool protobuf_field_cmp_fn(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* r) {
            int lv = (NULL == l)? 0: l->number();
            int rv = (NULL == r)? 0: r->number();
            return lv < rv;
        }
        */

        static void pick_const_data(const std::string& value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration& dur) {
            dur.set_seconds(0);
            dur.set_nanos(0);

            int64_t tm_val = 0;
            const char* word_begin = value.c_str();
            word_begin = skip_space(word_begin);
            word_begin = pick_number(tm_val, word_begin);
            word_begin = skip_space(word_begin);

            const char* word_end = value.c_str() + value.size();
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

        static void pick_const_data(const std::string& value, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp& timepoint) {
            timepoint.set_seconds(0);
            timepoint.set_nanos(0);

            const char* word_begin = value.c_str();
            word_begin = skip_space(word_begin);

            struct tm t;
            memset(&t, 0, sizeof(t));

            // year
            {
                word_begin = pick_number(t.tm_year, word_begin);
                word_begin = skip_space(word_begin);
                if (*word_begin == '-') {
                    ++ word_begin;
                    word_begin = skip_space(word_begin);
                }
                t.tm_year -= 1900; // years since 1900 
            }
            // month
            {
                word_begin = pick_number(t.tm_mon, word_begin);
                word_begin = skip_space(word_begin);
                if (*word_begin == '-') {
                    ++ word_begin;
                    word_begin = skip_space(word_begin);
                }

                -- t.tm_mon; // [0, 11] 
            }
            // day
            {
                word_begin = pick_number(t.tm_mday, word_begin);
                word_begin = skip_space(word_begin);
                if (*word_begin == 'T') { // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
                    ++ word_begin;
                    word_begin = skip_space(word_begin);
                }
            }

            // tm_hour
            {
                word_begin = pick_number(t.tm_hour, word_begin);
                word_begin = skip_space(word_begin);
                if (*word_begin == ':') { // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
                    ++ word_begin;
                    word_begin = skip_space(word_begin);
                }
            }

            // tm_min
            {
                word_begin = pick_number(t.tm_min, word_begin);
                word_begin = skip_space(word_begin);
                if (*word_begin == ':') { // skip T charactor, some format is YYYY-MM-DDThh:mm:ss
                    ++ word_begin;
                    word_begin = skip_space(word_begin);
                }
            }

             // tm_sec
            {
                word_begin = pick_number(t.tm_sec, word_begin);
                word_begin = skip_space(word_begin);
            }

            time_t res = mktime(&t);

            if (*word_begin == 'Z') { // UTC timezone
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

        static void dump_pick_field(const util::config::ini_value& val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds, size_t index) {
            if (NULL == fds) {
                return;
            }

            switch(fds->cpp_type()) {
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddInt32(&dst, fds, val.as_int32(index));
                    } else {
                        dst.GetReflection()->SetInt32(&dst, fds, val.as_int32(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddInt64(&dst, fds, val.as_int64(index));
                    } else {
                        dst.GetReflection()->SetInt64(&dst, fds, val.as_int64(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddUInt32(&dst, fds, val.as_uint32(index));
                    } else {
                        dst.GetReflection()->SetUInt32(&dst, fds, val.as_uint32(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddUInt64(&dst, fds, val.as_uint64(index));
                    } else {
                        dst.GetReflection()->SetUInt64(&dst, fds, val.as_uint64(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddString(&dst, fds, val.as_cpp_string(index));
                    } else {
                        dst.GetReflection()->SetString(&dst, fds, val.as_cpp_string(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
                    // special message
                    if (fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
                        const std::string& value = val.as_cpp_string(index);
                        if (fds->is_repeated()) {
                            parse_duration(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration*>(dst.GetReflection()->AddMessage(&dst, fds)));
                        } else {
                            parse_duration(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration*>(dst.GetReflection()->MutableMessage(&dst, fds)));
                        }
                        break;
                    } else if (fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
                        const std::string& value = val.as_cpp_string(index);
                        if (fds->is_repeated()) {
                            parse_timepoint(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp*>(dst.GetReflection()->AddMessage(&dst, fds)));
                        } else {
                            parse_timepoint(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp*>(dst.GetReflection()->MutableMessage(&dst, fds)));
                        }
                        break;
                    }

                    if (fds->is_repeated()) {
                        // repeated message is unpack by PARENT.0.field = XXX
                        for (uint32_t j = 0;; ++j) {
                            std::string idx = LOG_WRAPPER_FWAPI_FORMAT("{}", j);
                            util::config::ini_value::node_type::const_iterator idx_iter = val.get_children().find(idx);
                            if (idx_iter == val.get_children().end()) {
                                break;
                            }

                            ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message* submsg = dst.GetReflection()->AddMessage(&dst, fds);
                            if (NULL != submsg) {
                                ini_loader_dump_to(idx_iter->second, *submsg);
                            }
                        }
                        break;
                    } else {
                        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message* submsg = dst.GetReflection()->MutableMessage(&dst, fds);
                        if (NULL != submsg) {
                            ini_loader_dump_to(val, *submsg);
                        }
                    }
                    
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddDouble(&dst, fds, val.as_double(index));
                    } else {
                        dst.GetReflection()->SetDouble(&dst, fds, val.as_double(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddFloat(&dst, fds, val.as_float(index));
                    } else {
                        dst.GetReflection()->SetFloat(&dst, fds, val.as_float(index));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
                    bool jval = true;
                    std::string trans = val.as_cpp_string(index);
                    std::transform(trans.begin(), trans.end(), trans.begin(), util::string::tolower<char>);

                    if ("0" == trans || "false" == trans || "no" == trans || "disable" == trans || "disabled" == trans || "" == trans) {
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
                    const std::string& name = val.as_cpp_string(index);
                    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor* jval = NULL;
                    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) {
                        jval = fds->enum_type()->FindValueByNumber(val.as_int32(index));
                    } else {
                        jval = fds->enum_type()->FindValueByName(name);
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
                    FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(), fds->type_name());
                    break;
                }
            }
        }

        static void dump_field_item(const util::config::ini_value& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds) {
            if (NULL == fds) {
                return;
            }

            util::config::ini_value::node_type::const_iterator child_iter = src.get_children().find(fds->name());
            // skip if not found, just skip
            if (child_iter == src.get_children().end()) {
                return;
            }

            if (fds->is_repeated() && fds->cpp_type() != ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE) {
                size_t arrsz = child_iter->second.size();
                for (size_t i = 0; i < arrsz; ++ i) {
                    dump_pick_field(child_iter->second, dst, fds, i);
                }     
            } else {

                dump_pick_field(child_iter->second, dst, fds, 0);
            }
        }

        static void dump_message_item(const YAML::Node& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst);
        static void dump_pick_field(const YAML::Node& val, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds) {
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

            switch(fds->cpp_type()) {
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddInt32(&dst, fds, util::string::to_int<int32_t>(val.Scalar().c_str()));
                    } else {
                        dst.GetReflection()->SetInt32(&dst, fds, util::string::to_int<int32_t>(val.Scalar().c_str()));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT64: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddInt64(&dst, fds, util::string::to_int<int64_t>(val.Scalar().c_str()));
                    } else {
                        dst.GetReflection()->SetInt64(&dst, fds, util::string::to_int<int64_t>(val.Scalar().c_str()));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT32: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddUInt32(&dst, fds, util::string::to_int<uint32_t>(val.Scalar().c_str()));
                    } else {
                        dst.GetReflection()->SetUInt32(&dst, fds, util::string::to_int<uint32_t>(val.Scalar().c_str()));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_UINT64: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddUInt64(&dst, fds, util::string::to_int<uint64_t>(val.Scalar().c_str()));
                    } else {
                        dst.GetReflection()->SetUInt64(&dst, fds, util::string::to_int<uint64_t>(val.Scalar().c_str()));
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_STRING: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddString(&dst, fds, val.Scalar());
                    } else {
                        dst.GetReflection()->SetString(&dst, fds, val.Scalar());
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_MESSAGE: {
                    // special message
                    if (val.IsScalar()) {
                        if (fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration::descriptor()->full_name()) {
                            const std::string& value = val.Scalar();
                            if (fds->is_repeated()) {
                                parse_duration(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration*>(dst.GetReflection()->AddMessage(&dst, fds)));
                            } else {
                                parse_duration(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration*>(dst.GetReflection()->MutableMessage(&dst, fds)));
                            }
                            
                        } else if (fds->message_type()->full_name() == ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp::descriptor()->full_name()) {
                            const std::string& value = val.Scalar();
                            if (fds->is_repeated()) {
                                parse_timepoint(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp*>(dst.GetReflection()->AddMessage(&dst, fds)));
                            } else {
                                parse_timepoint(value, *static_cast<ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp*>(dst.GetReflection()->MutableMessage(&dst, fds)));
                            }
                        }

                        break;
                    }

                    if (val.IsMap()) {
                        ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message* submsg;
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
                    if (!val.IsScalar()) {
                        break;
                    }

                    std::stringstream s_stream;
                    double res;
                    s_stream.str(val.Scalar());
                    s_stream >> res;

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddDouble(&dst, fds, res);
                    } else {
                        dst.GetReflection()->SetDouble(&dst, fds, res);
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_FLOAT: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    std::stringstream s_stream;
                    float res;
                    s_stream.str(val.Scalar());
                    s_stream >> res;

                    if (fds->is_repeated()) {
                        dst.GetReflection()->AddFloat(&dst, fds, res);
                    } else {
                        dst.GetReflection()->SetFloat(&dst, fds, res);
                    }
                    break;
                };
                case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_BOOL: {
                    if (!val.IsScalar()) {
                        break;
                    }

                    bool jval = true;
                    std::string trans = val.Scalar();
                    std::transform(trans.begin(), trans.end(), trans.begin(), util::string::tolower<char>);

                    if ("0" == trans || "false" == trans || "no" == trans || "disable" == trans || "disabled" == trans || "" == trans) {
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

                    const std::string& name = val.Scalar();
                    const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::EnumValueDescriptor* jval = NULL;
                    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) {
                        jval = fds->enum_type()->FindValueByNumber(util::string::to_int<int32_t>(name.c_str()));
                    } else {
                        jval = fds->enum_type()->FindValueByName(name);
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
                    FWLOGERROR("{} in {} with type={} is not supported now", fds->name(), dst.GetDescriptor()->full_name(), fds->type_name());
                    break;
                }
            }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
            } catch(...) {
                // Ignore error
            }
#endif
        }

        static void dump_field_item(const YAML::Node& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds) {
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
                    for (YAML::Node::const_iterator child_iter = child.begin(); child_iter != child.end(); ++ child_iter) {
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
            } catch(...) {
                // Ignore error
            }
#endif
        }

        static void dump_message_item(const YAML::Node& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst) {
            const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor* desc = dst.GetDescriptor();
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
            } catch(...) {
                // Ignore error
            }
#endif
            for (int i = 0 ;i < desc->field_count(); ++ i) {
                detail::dump_field_item(src, dst, desc->field(i));
            }
        }
    }
    
    LIBATAPP_MACRO_API void parse_timepoint(const std::string& in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Timestamp& out) {
        detail::pick_const_data(in, out);
    }

    LIBATAPP_MACRO_API void parse_duration(const std::string& in, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Duration& out) {
        detail::pick_const_data(in, out);
    }

    LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst) {
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor* desc = dst.GetDescriptor();
        if (NULL == desc) {
            return;
        }

        for (int i = 0 ;i < desc->field_count(); ++ i) {
            detail::dump_field_item(src, dst, desc->field(i));
        }
    }

    LIBATAPP_MACRO_API void ini_loader_dump_to(const util::config::ini_value& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string>& dst, std::string prefix) {
        if (src.size() > 0) {
            if (1 == src.size()) {
                dst[prefix] = src.as_cpp_string();
            } else {
                for (size_t i = 0; i < src.size(); ++ i) {
                    std::string sub_prefix = prefix.empty()? LOG_WRAPPER_FWAPI_FORMAT("{}", i): LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, i);
                    dst[sub_prefix] = src.as_cpp_string(i);
                }
            }
        }

        for (util::config::ini_value::node_type::const_iterator iter = src.get_children().begin(); iter != src.get_children().end(); ++ iter) {
            // First level skip fields already in ::atapp::protocol::atapp_log_sink
            if (prefix.empty()) {
                const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds = ::atapp::protocol::atapp_log_sink::descriptor()->FindFieldByName(iter->first);
                if (NULL != fds && fds->name() != "unresolved_key_values") {
                    continue;
                }
            }

            std::string sub_prefix = prefix.empty()? iter->first: LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, iter->first);
            ini_loader_dump_to(iter->second, dst, sub_prefix);
        }
    }

    LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& dst) {
        detail::dump_message_item(src, dst);
    }

    LIBATAPP_MACRO_API void yaml_loader_dump_to(const YAML::Node& src, ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Map<std::string, std::string>& dst, std::string prefix) {
#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
        try {
#endif
        if (!src) {
            return;
        }

        if (src.IsScalar()) {
            dst[prefix] = src.Scalar();
        } else if (src.IsMap()) {
            for (YAML::Node::const_iterator iter = src.begin(); iter != src.end(); ++ iter) {
                if (!iter->first || !iter->first.IsScalar()) {
                    continue;
                }

                std::string sub_prefix = prefix.empty()? iter->first.Scalar(): LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, iter->first.Scalar());
                yaml_loader_dump_to(iter->second, dst, sub_prefix);
            }
        } else if (src.IsSequence()) {
            if (1 == src.size()) {
                yaml_loader_dump_to(src[0], dst, prefix);
            } else {
                for (size_t i = 0; i < src.size(); ++ i) {
                    std::string sub_prefix = prefix.empty()? LOG_WRAPPER_FWAPI_FORMAT("{}", i): LOG_WRAPPER_FWAPI_FORMAT("{}.{}", prefix, i);
                    yaml_loader_dump_to(src[i], dst, sub_prefix);
                }
            }
        }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
        } catch(...) {
            // Ignore error
        }
#endif
    }

    LIBATAPP_MACRO_API const YAML::Node yaml_loader_get_child_by_path(const YAML::Node& src, const std::string& path) {
        if (path.empty()) {
            return src;
        }

        if (!src) {
            return src;
        }

#if defined(LIBATFRAME_UTILS_ENABLE_EXCEPTION) && LIBATFRAME_UTILS_ENABLE_EXCEPTION
        try {
#endif

        YAML::Node ret    = src;
        const char *begin = path.c_str();
        const char *end = begin + path.size();

        for (begin = detail::skip_space(begin); end > begin && *begin; begin = detail::skip_space(begin)) {
            const char *old_begin = begin;
            ++ begin;
            while (*begin && end > begin && '.' != *begin && ' ' != *begin && '\t' != *begin && '\r' != *begin && '\n' != *begin) {
                ++ begin;
            }

            std::string key;
            key.assign(old_begin, begin);

            if (*begin) {
                begin = detail::skip_space(path.c_str());
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
        } catch(...) {
            // Ignore error
        }
#endif

        return YAML::Node(YAML::NodeType::Undefined);
    }

    static bool protobuf_equal_inner_message(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& r);
    static bool protobuf_equal_inner_field(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& r, 
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds);


    bool protobuf_equal_inner_message(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& r) {
        if (&l == &r) {
            return true;
        }

        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Descriptor* desc = l.GetDescriptor();
        if (desc != r.GetDescriptor()) {
            return false;
        }

        for (int i = 0; i < desc->field_count(); ++ i) {
            if (false == protobuf_equal_inner_field(l, r, desc->field(i))) {
                return false;
            }
        }

        return true;
    }

    bool protobuf_equal_inner_field(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& r, 
        const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor* fds) {

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
        
        switch(fds->cpp_type()) {
            case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_INT32: {
                if (fds->is_repeated()) {
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
                        if (false == protobuf_equal_inner_message(
                            l.GetReflection()->GetRepeatedMessage(l, fds, i), 
                            r.GetReflection()->GetRepeatedMessage(r, fds, i)
                        )) {
                            return false;
                        }
                    }
                } else {
                    return protobuf_equal_inner_message(l.GetReflection()->GetMessage(l, fds), r.GetReflection()->GetMessage(r, fds));
                }
                break;
            };
            case ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::FieldDescriptor::CPPTYPE_DOUBLE: {
                if (fds->is_repeated()) {
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
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
                    for (int i = 0; i < field_size; ++ i)  {
                        if (l.GetReflection()->GetRepeatedEnumValue(l, fds, i) != r.GetReflection()->GetRepeatedEnumValue(r, fds, i)) {
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

    LIBATAPP_MACRO_API bool protobuf_equal(const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& l, const ATBUS_MACRO_PROTOBUF_NAMESPACE_ID::Message& r) {
        return protobuf_equal_inner_message(l, r);
    }
}