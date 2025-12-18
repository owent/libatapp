// Copyright 2021 atframework
// Created by owent

#include <libatbus.h>

#include <algorithm/base64.h>

#include <algorithm/murmur_hash.h>
#include <common/string_oprs.h>
#include <log/log_wrapper.h>
#include <random/random_generator.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_watcher.h>

#include <memory>

#ifdef GetObject
#  undef GetObject
#endif

LIBATAPP_MACRO_NAMESPACE_BEGIN

LIBATAPP_MACRO_API etcd_watcher::etcd_watcher(etcd_cluster &owner, const std::string &path,
                                              const std::string &range_end, constrict_helper_t &)
    : owner_(&owner), path_(path), range_end_(range_end), rpc_data_brackets_(0) {
  rpc_.retry_interval = std::chrono::seconds(15);      // 重试间隔15秒
  rpc_.request_timeout = std::chrono::hours(1);        // 一小时超时时间，相当于每小时重新拉取数据
  rpc_.get_request_timeout = std::chrono::minutes(3);  // 3分钟超时时间，这个数据量可能很大，需要单独设置超时
  rpc_.startup_random_delay_min = std::chrono::system_clock::duration::zero();  // 随机启动延迟
  rpc_.startup_random_delay_max = std::chrono::system_clock::duration::zero();  // 随机启动延迟
  rpc_.watcher_next_request_time = std::chrono::system_clock::from_time_t(0);
  rpc_.enable_progress_notify = true;
  rpc_.enable_prev_kv = false;
  rpc_.is_actived = false;
  rpc_.is_retry_mode = false;
  rpc_.last_revision = 0;
}

LIBATAPP_MACRO_API etcd_watcher::~etcd_watcher() { close(); }

LIBATAPP_MACRO_API etcd_watcher::ptr_t etcd_watcher::create(etcd_cluster &owner, const std::string &path,
                                                            const std::string &range_end) {
  constrict_helper_t h;
  return std::make_shared<etcd_watcher>(owner, path, range_end, h);
}

LIBATAPP_MACRO_API void etcd_watcher::close() {
  if (rpc_.rpc_opr_) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*owner_, "Etcd watcher {} cancel http request.",
                                          reinterpret_cast<const void *>(this));
    rpc_.rpc_opr_->set_on_complete(nullptr);
    rpc_.rpc_opr_->set_on_write(nullptr);
    rpc_.rpc_opr_->set_priv_data(nullptr);
    rpc_.rpc_opr_->stop();
    rpc_.rpc_opr_.reset();
  }
  rpc_.is_actived = false;
  rpc_.is_retry_mode = false;
  rpc_.last_revision = 0;

  // destroy watcher handle
  evt_handle_ = nullptr;
}

LIBATAPP_MACRO_API const std::string &etcd_watcher::get_path() const { return path_; }

LIBATAPP_MACRO_API void etcd_watcher::active() {
  rpc_.is_actived = true;
  process();
}

void etcd_watcher::process() {
  if (rpc_.rpc_opr_) {
    return;
  }

  rpc_.is_actived = false;

  if (rpc_.watcher_next_request_time > atfw::util::time::time_utility::sys_now()) {
    return;
  }

  // Special delay on startup
  if (rpc_.watcher_next_request_time <= std::chrono::system_clock::from_time_t(0)) {
    atfw::util::time::time_utility::update();
    if (rpc_.startup_random_delay_min > std::chrono::system_clock::duration::zero()) {
      if (rpc_.startup_random_delay_max > rpc_.startup_random_delay_min) {
        char buffer[sizeof(int) + sizeof(time_t)];
        int pid = atbus::node::get_pid();
        memcpy(&buffer[0], &pid, sizeof(pid));
        time_t now_usec =
            atfw::util::time::time_utility::get_sys_now() * 1000000 + atfw::util::time::time_utility::get_now_usec();
        memcpy(&buffer[sizeof(pid)], &now_usec, sizeof(now_usec));

        uint32_t seed = atfw::util::hash::murmur_hash3_x86_32(buffer, sizeof(buffer), LIBATAPP_MACRO_HASH_MAGIC_NUMBER);
        atfw::util::random::xoshiro256_starstar random_generator{
            static_cast<atfw::util::random::xoshiro256_starstar::result_type>(seed)};
        int64_t offset = (rpc_.startup_random_delay_max - rpc_.startup_random_delay_min).count();
        std::chrono::system_clock::duration select_offset =
            std::chrono::system_clock::duration(random_generator.random_between<int64_t>(0, offset));
        rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + select_offset;
      } else {
        rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + rpc_.startup_random_delay_min;
      }

      auto delay_timepoint =
          std::chrono::duration_cast<std::chrono::microseconds>(rpc_.watcher_next_request_time.time_since_epoch())
              .count();
      FWLOGINFO("Etcd watcher {} delay range {} to {}.{}", reinterpret_cast<const void *>(this),
                rpc_.rpc_opr_->get_url(), (delay_timepoint / 1000000), delay_timepoint % 1000000);
      return;
    }
  }

  // ask for revision first
  if (0 == rpc_.last_revision || rpc_.is_retry_mode) {
    // create range request

    if (rpc_.is_retry_mode) {
      rpc_.rpc_opr_ = owner_->create_request_kv_get(path_, "");
    } else {
      rpc_.rpc_opr_ = owner_->create_request_kv_get(path_, range_end_);
    }
    if (!rpc_.rpc_opr_) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*owner_, "Etcd watcher {} create range request to {} failed",
                                            reinterpret_cast<const void *>(this), path_);
      rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + rpc_.retry_interval;
      return;
    }

    if (!rpc_.is_retry_mode && rpc_.get_request_timeout > std::chrono::milliseconds::zero()) {
      rpc_.rpc_opr_->set_opt_timeout(
          static_cast<time_t>(std::chrono::duration_cast<std::chrono::milliseconds>(rpc_.get_request_timeout).count()));
    }

    rpc_.rpc_opr_->set_priv_data(this);
    rpc_.rpc_opr_->set_on_complete(libcurl_callback_on_range_completed);

    int res = rpc_.rpc_opr_->start(atfw::util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      rpc_.rpc_opr_->set_on_complete(nullptr);
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*owner_, "Etcd watcher {} start request to {} failed, res: {}",
                                            reinterpret_cast<const void *>(this), rpc_.rpc_opr_->get_url(), res);
      rpc_.rpc_opr_.reset();
    } else {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*owner_, "Etcd watcher {} start request to {} success.",
                                            reinterpret_cast<const void *>(this), rpc_.rpc_opr_->get_url());
    }

    return;
  }

  // create watcher request for next resision
  rpc_.rpc_opr_ = owner_->create_request_watch(path_, range_end_, rpc_.last_revision + 1, rpc_.enable_prev_kv,
                                               rpc_.enable_progress_notify);
  if (!rpc_.rpc_opr_) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*owner_, "Etcd watcher {} create watch request to {} from revision {} failed",
                                          reinterpret_cast<const void *>(this), path_, rpc_.last_revision + 1);
    rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + rpc_.retry_interval;
    return;
  }

  rpc_.rpc_opr_->set_priv_data(this);
  rpc_.rpc_opr_->set_on_complete(libcurl_callback_on_watch_completed);
  rpc_.rpc_opr_->set_on_write(libcurl_callback_on_watch_write);
  rpc_.rpc_opr_->set_opt_timeout(
      static_cast<time_t>(std::chrono::duration_cast<std::chrono::milliseconds>(rpc_.request_timeout).count()));

  rpc_data_stream_.str("");
  rpc_data_brackets_ = 0;

  int res = rpc_.rpc_opr_->start(atfw::util::network::http_request::method_t::EN_MT_POST, false);
  if (res != 0) {
    rpc_.rpc_opr_->set_on_complete(nullptr);
    rpc_.rpc_opr_->set_on_write(nullptr);
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
        *owner_, "Etcd watcher {} start watch request to {} from revision {} failed, res: {}",
        reinterpret_cast<const void *>(this), rpc_.rpc_opr_->get_url(), rpc_.last_revision + 1, res);
    rpc_.rpc_opr_.reset();
  } else {
    FWLOGINFO("Etcd watcher {} start watch request to {} from revision {} success.",
              reinterpret_cast<const void *>(this), rpc_.rpc_opr_->get_url(), rpc_.last_revision + 1);
  }

  return;
}

int etcd_watcher::libcurl_callback_on_range_completed(atfw::util::network::http_request &req) {
  etcd_watcher *self = reinterpret_cast<etcd_watcher *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd watcher range request shouldn't has request without private data");
    return 0;
  }
  atfw::util::network::http_request::ptr_t keep_rpc = self->rpc_.rpc_opr_;
  self->rpc_.rpc_opr_.reset();

  // 服务器错误则过一段时间后重试
  if (0 != req.get_error_code() ||
      atfw::util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
          atfw::util::network::http_request::get_status_code_group(req.get_response_code())) {
    std::string response_content = req.get_response_stream().str();
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self->owner_,
                                          "Etcd watcher {} range request failed, error code: {}, http code: {}\n{}\n{}",
                                          reinterpret_cast<const void *>(self), req.get_error_code(),
                                          req.get_response_code(), req.get_error_msg(), response_content);

    self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + self->rpc_.retry_interval;

    self->owner_->check_socket_error_code(req.get_error_code());
    self->owner_->check_authorization_expired(req.get_response_code(), response_content);
    return 0;
  }

  // 重试是模式是为了触发一下可能需要的token替换
  if (self->rpc_.is_retry_mode) {
    self->rpc_.is_retry_mode = false;
    // reset request time to invoke watch request immediately
    self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now();

    // 立刻开启下一次watch
    self->active();
    return 0;
  }

  std::string http_content;
  req.get_response_stream().str().swap(http_content);
  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self->owner_, "Etcd watcher {} got range http response: {}",
                                        reinterpret_cast<const void *>(self), http_content);

  rapidjson::Document doc;
  if (false == atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self->owner_, "Etcd watcher {} got range response parse failed: {}",
                                          reinterpret_cast<const void *>(self), http_content);

    self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + self->rpc_.retry_interval;
    self->active();
    return 0;
  }

  // unpack header
  etcd_response_header header;
  {
    header.revision = 0;
    rapidjson::Document::ConstMemberIterator res = doc.FindMember("header");
    if (res != doc.MemberEnd()) {
      etcd_packer::unpack(header, res->value);
    }
  }

  if (0 == header.revision) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self->owner_, "Etcd watcher {} got range response without header",
                                          reinterpret_cast<const void *>(self));

    self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + self->rpc_.retry_interval;
    self->active();
    return 0;
  }

  // save revision
  self->rpc_.last_revision = header.revision;

  // first event
  response_t response;
  response.watch_id = 0;
  response.created = false;
  response.canceled = false;
  response.snapshot = true;
  response.compact_revision = 0;
  {
    rapidjson::Document::ConstMemberIterator res = doc.FindMember("kvs");

    if (doc.MemberEnd() != res) {
      uint64_t reverse_sz = 0;
      etcd_packer::unpack_int(doc, "count", reverse_sz);
      if (0 == reverse_sz) {
        reverse_sz = 64;
      }
      response.events.reserve(static_cast<size_t>(reverse_sz));

      if (res->value.IsArray()) {
        rapidjson::Document::ConstArray all_events = res->value.GetArray();
        for (rapidjson::Document::Array::ConstValueIterator iter = all_events.Begin(); iter != all_events.End();
             ++iter) {
          response.events.push_back(event_t());
          event_t &evt = response.events.back();

          evt.evt_type = etcd_watch_event::EN_WEVT_PUT;  // 查询的结果都认为是PUT
          etcd_packer::unpack(evt.kv, *iter);
        }
      }
    }
  }

  if (atfw::util::log::log_wrapper::check_level(WDTLOGGETCAT(atfw::util::log::log_wrapper::categorize_t::DEFAULT),
                                                atfw::util::log::log_wrapper::level_t::LOG_LW_DEBUG)) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self->owner_, "Etcd watcher {} got range response",
                                          reinterpret_cast<const void *>(self));
    for (size_t i = 0; i < response.events.size(); ++i) {
      etcd_key_value *kv = &response.events[i].kv;
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self->owner_, "    InitEvt => type: PUT, key: {}, value: {}", kv->key,
                                            kv->value);
    }
  }

  // trigger event
  if (self->evt_handle_) {
    self->evt_handle_(header, response);
  }

  // reset request time to invoke watch request immediately
  self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now();

  // 立刻开启下一次watch
  self->active();
  return 0;
}

int etcd_watcher::libcurl_callback_on_watch_completed(atfw::util::network::http_request &req) {
  etcd_watcher *self = reinterpret_cast<etcd_watcher *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd watcher watch request shouldn't has request without private data");
    return 0;
  }
  atfw::util::network::http_request::ptr_t keep_rpc = self->rpc_.rpc_opr_;
  self->rpc_.rpc_opr_.reset();
  self->rpc_.is_retry_mode = true;

  // 服务器错误则过一段时间后重试
  if (0 != req.get_error_code() ||
      atfw::util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
          atfw::util::network::http_request::get_status_code_group(req.get_response_code())) {
    // timeout是正常的保活流程
    if (CURLE_OPERATION_TIMEDOUT != req.get_error_code() &&
        atfw::util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
            atfw::util::network::http_request::get_status_code_group(req.get_response_code())) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(
          *self->owner_, "Etcd watcher {} watch request failed, error code: {}, http code: {}\n{}\n{}",
          reinterpret_cast<const void *>(self), req.get_error_code(), req.get_response_code(), req.get_error_msg(),
          req.get_response_stream().str());

      self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now() + self->rpc_.retry_interval;

    } else {
      FWLOGINFO("Etcd watcher {} watch request finished, start another request later, msg: {}.\n{}",
                reinterpret_cast<const void *>(self), req.get_error_msg(), req.get_response_stream().str());
      self->rpc_.watcher_next_request_time = atfw::util::time::time_utility::sys_now();
    }

    self->owner_->check_socket_error_code(req.get_error_code());
    self->owner_->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());

    // 立刻开启下一次watch
    self->active();
    return 0;
  }

  LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self->owner_, "Etcd watcher {} got watch http response",
                                        reinterpret_cast<const void *>(self));

  // 立刻开启下一次watch
  self->active();
  return 0;
}

int etcd_watcher::libcurl_callback_on_watch_write(atfw::util::network::http_request &req, const char *inbuf,
                                                  size_t inbufsz, const char *&outbuf, size_t &outbufsz) {
  // etcd_watcher 模块内消耗掉缓冲区，不需要写出到通用缓冲区了
  outbuf = nullptr;
  outbufsz = 0;

  etcd_watcher *self = reinterpret_cast<etcd_watcher *>(req.get_priv_data());
  if (nullptr == self) {
    FWLOGERROR("Etcd watcher watch request shouldn't has request without private data");
    return 0;
  }

  if (inbuf == nullptr || 0 == inbufsz) {
    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self->owner_, "Etcd watcher {} got http trunk without data",
                                          reinterpret_cast<const void *>(self));
    return 0;
  }

  while (inbufsz > 0) {
    bool need_process = false;
    // etcd 的汇报数据内容里内有符号，所以这里直接用括号匹配来判定
    int64_t rpc_data_brackets = self->rpc_data_brackets_;
    if (rpc_data_brackets <= 0) {
      while (inbufsz > 0 && inbuf[0]) {
        if (inbuf[0] == '{' || inbuf[0] == '[') {
          break;
        }

        --inbufsz;
        ++inbuf;
      }
    }

    for (size_t i = 0; i < inbufsz; ++i) {
      if (inbuf[i] == '{' || inbuf[i] == '[') {
        ++rpc_data_brackets;
      }

      if (inbuf[i] == '}' || inbuf[i] == ']') {
        --rpc_data_brackets;
      }

      if (rpc_data_brackets <= 0) {
        self->rpc_data_stream_.write(inbuf, i + 1);
        inbuf += i + 1;
        inbufsz -= i + 1;
        need_process = true;
        break;
      }
    }

    if (!need_process) {
      self->rpc_data_stream_.write(inbuf, inbufsz);
      self->rpc_data_brackets_ = rpc_data_brackets;
      break;
    }

    // 如果lease不存在（没有TTL）则启动创建流程
    rapidjson::Document doc;
    std::string value_json;
    self->rpc_data_stream_.str().swap(value_json);
    self->rpc_data_stream_.str("");
    self->rpc_data_brackets_ = 0;

    LIBATAPP_MACRO_ETCD_CLUSTER_LOG_TRACE(*self->owner_, "Etcd watcher {} got http trunk: {}",
                                          reinterpret_cast<const void *>(self), value_json);
    // 忽略空数据
    if (false == atapp::etcd_packer::parse_object(doc, value_json.c_str())) {
      continue;
    }

    rapidjson::Value &root = doc;
    const rapidjson::Value *result = &root;
    {
      rapidjson::Document::ConstMemberIterator res = root.FindMember("result");
      if (res != root.MemberEnd()) {
        result = &res->value;
      }
    }

    // unpack header
    int64_t previous_revision = self->rpc_.last_revision;
    etcd_response_header header;
    {
      rapidjson::Document::ConstMemberIterator res = result->FindMember("header");
      if (res != result->MemberEnd()) {
        etcd_packer::unpack(header, res->value);
        // save revision
        if (0 != header.revision) {
          self->rpc_.last_revision = header.revision;
        }
      } else {
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self->owner_, "Etcd watcher {} got http trunk without header",
                                              reinterpret_cast<const void *>(self));
      }
    }

    response_t response;
    response.snapshot = false;
    // decode basic info
    etcd_packer::unpack_int(*result, "watch_id", response.watch_id);
    etcd_packer::unpack_int(*result, "compact_revision", response.compact_revision);
    etcd_packer::unpack_bool(*result, "created", response.created);
    etcd_packer::unpack_bool(*result, "canceled", response.canceled);

    rapidjson::Document::ConstMemberIterator events = result->FindMember("events");
    if (result->MemberEnd() != events && events->value.IsArray()) {
      rapidjson::Document::ConstArray all_events = events->value.GetArray();
      for (rapidjson::Document::Array::ConstValueIterator iter = all_events.Begin(); iter != all_events.End(); ++iter) {
        response.events.push_back(event_t());
        event_t &evt = response.events.back();

        rapidjson::Document::ConstMemberIterator type = iter->FindMember("type");
        if (type == iter->MemberEnd()) {
          evt.evt_type = etcd_watch_event::EN_WEVT_PUT;  // etcd可能不会下发默认值
        } else {
          if (type->value.IsString()) {
            if (0 == UTIL_STRFUNC_STRCASE_CMP("DELETE", type->value.GetString())) {
              evt.evt_type = etcd_watch_event::EN_WEVT_DELETE;
            } else {
              evt.evt_type = etcd_watch_event::EN_WEVT_PUT;
            }
          } else if (type->value.IsNumber()) {
            uint64_t type_int = 0;
            etcd_packer::unpack_int(*iter, "type", type_int);
            if (0 == type_int) {
              evt.evt_type = etcd_watch_event::EN_WEVT_PUT;
            } else {
              evt.evt_type = etcd_watch_event::EN_WEVT_DELETE;
            }
          } else {
            LIBATAPP_MACRO_ETCD_CLUSTER_LOG_ERROR(*self->owner_, "Etcd watcher {} got unknown event type. msg: {}",
                                                  reinterpret_cast<const void *>(self), value_json);
          }
        }

        rapidjson::Document::ConstMemberIterator kv = iter->FindMember("kv");
        if (kv != iter->MemberEnd()) {
          etcd_packer::unpack(evt.kv, kv->value);
        }

        rapidjson::Document::ConstMemberIterator prev_kv = iter->FindMember("prev_kv");
        if (prev_kv != iter->MemberEnd()) {
          etcd_packer::unpack(evt.prev_kv, prev_kv->value);
        }
      }
    }

    if (atfw::util::log::log_wrapper::check_level(WDTLOGGETCAT(atfw::util::log::log_wrapper::categorize_t::DEFAULT),
                                                  atfw::util::log::log_wrapper::level_t::LOG_LW_DEBUG)) {
      LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(
          *self->owner_,
          "Etcd watcher {} got response: watch_id: {}, compact_revision: {}, created: {}, canceled: {}, event: {}",
          reinterpret_cast<const void *>(self), response.watch_id, response.compact_revision,
          response.created ? "Yes" : "No", response.canceled ? "Yes" : "No", response.events.size());
      for (size_t i = 0; i < response.events.size(); ++i) {
        etcd_key_value *kv = &response.events[i].kv;
        const char *name;
        if (etcd_watch_event::EN_WEVT_PUT == response.events[i].evt_type) {
          name = "PUT";
        } else {
          name = "DELETE";
        }
        LIBATAPP_MACRO_ETCD_CLUSTER_LOG_DEBUG(*self->owner_, "    Evt => type: {}, key: {}, value: {}", name, kv->key,
                                              kv->value);
      }
    }

    // trigger event
    if (self->evt_handle_) {
      self->evt_handle_(header, response);
    }

    // stopped if canceled and wait to start another watcher later
    if (response.canceled) {
      std::string cancel_reason;
      etcd_packer::unpack_string(*result, "cancel_reason", cancel_reason);
      FWLOGINFO(
          "Etcd watcher {} got cancel response: watch_id: {}, previous_revision: {}, compact_revision: {}, "
          "cancel_reason: {}",
          reinterpret_cast<const void *>(self), response.watch_id, previous_revision, response.compact_revision,
          cancel_reason);

      // Watch revision compacted, must get data by range again
      if (previous_revision < response.compact_revision) {
        self->rpc_.last_revision = 0;
      }

      req.stop();
    }
  }

  return 0;
}

LIBATAPP_MACRO_NAMESPACE_END
