#include <libatbus.h>

#include <algorithm/base64.h>

#include <common/string_oprs.h>
#include <log/log_wrapper.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_watcher.h>

#ifdef GetObject
#  undef GetObject
#endif

namespace atapp {

LIBATAPP_MACRO_API etcd_watcher::etcd_watcher(etcd_cluster &owner, const std::string &path,
                                              const std::string &range_end, constrict_helper_t &)
    : owner_(&owner), path_(path), range_end_(range_end), rpc_data_brackets_(0) {
  rpc_.retry_interval = std::chrono::seconds(15);  // 重试间隔15秒
  rpc_.request_timeout = std::chrono::hours(1);    // 一小时超时时间，相当于每小时重新拉取数据
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
    FWLOGDEBUG("Etcd watcher {} cancel http request.", reinterpret_cast<const void *>(this));
    rpc_.rpc_opr_->set_on_complete(NULL);
    rpc_.rpc_opr_->set_on_write(NULL);
    rpc_.rpc_opr_->set_priv_data(NULL);
    rpc_.rpc_opr_->stop();
    rpc_.rpc_opr_.reset();
  }
  rpc_.is_actived = false;
  rpc_.is_retry_mode = false;
  rpc_.last_revision = 0;
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

  if (rpc_.watcher_next_request_time > util::time::time_utility::sys_now()) {
    return;
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
      FWLOGERROR("Etcd watcher {} create range request to {} failed", reinterpret_cast<const void *>(this), path_);
      rpc_.watcher_next_request_time = util::time::time_utility::sys_now() + rpc_.retry_interval;
      return;
    }

    rpc_.rpc_opr_->set_priv_data(this);
    rpc_.rpc_opr_->set_on_complete(libcurl_callback_on_range_completed);

    int res = rpc_.rpc_opr_->start(util::network::http_request::method_t::EN_MT_POST, false);
    if (res != 0) {
      rpc_.rpc_opr_->set_on_complete(NULL);
      FWLOGERROR("Etcd watcher {} start request to {} failed, res: {}", reinterpret_cast<const void *>(this),
                 rpc_.rpc_opr_->get_url(), res);
      rpc_.rpc_opr_.reset();
    } else {
      FWLOGDEBUG("Etcd watcher {} start request to {} success.", reinterpret_cast<const void *>(this),
                 rpc_.rpc_opr_->get_url());
    }

    return;
  }

  // create watcher request for next resision
  rpc_.rpc_opr_ = owner_->create_request_watch(path_, range_end_, rpc_.last_revision + 1, rpc_.enable_prev_kv,
                                               rpc_.enable_progress_notify);
  if (!rpc_.rpc_opr_) {
    FWLOGERROR("Etcd watcher {} create watch request to {} failed", reinterpret_cast<const void *>(this), path_);
    rpc_.watcher_next_request_time = util::time::time_utility::sys_now() + rpc_.retry_interval;
    return;
  }

  rpc_.rpc_opr_->set_priv_data(this);
  rpc_.rpc_opr_->set_on_complete(libcurl_callback_on_watch_completed);
  rpc_.rpc_opr_->set_on_write(libcurl_callback_on_watch_write);
  rpc_.rpc_opr_->set_opt_timeout(
      static_cast<time_t>(std::chrono::duration_cast<std::chrono::milliseconds>(rpc_.request_timeout).count()));

  rpc_data_stream_.str("");
  rpc_data_brackets_ = 0;

  int res = rpc_.rpc_opr_->start(util::network::http_request::method_t::EN_MT_POST, false);
  if (res != 0) {
    rpc_.rpc_opr_->set_on_complete(NULL);
    rpc_.rpc_opr_->set_on_write(NULL);
    FWLOGERROR("Etcd watcher {} start request to {} failed, res: {}", reinterpret_cast<const void *>(this),
               rpc_.rpc_opr_->get_url(), res);
    rpc_.rpc_opr_.reset();
  } else {
    FWLOGDEBUG("Etcd watcher {} start request to {} success.", reinterpret_cast<const void *>(this),
               rpc_.rpc_opr_->get_url());
  }

  return;
}

int etcd_watcher::libcurl_callback_on_range_completed(util::network::http_request &req) {
  etcd_watcher *self = reinterpret_cast<etcd_watcher *>(req.get_priv_data());
  if (NULL == self) {
    FWLOGERROR("Etcd watcher range request shouldn't has request without private data");
    return 0;
  }
  util::network::http_request::ptr_t keep_rpc = self->rpc_.rpc_opr_;
  self->rpc_.rpc_opr_.reset();

  // 服务器错误则过一段时间后重试
  if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                       util::network::http_request::get_status_code_group(req.get_response_code())) {
    FWLOGERROR("Etcd watcher {} range request failed, error code: {}, http code: {}\n{}",
               reinterpret_cast<const void *>(self), req.get_error_code(), req.get_response_code(),
               req.get_error_msg());

    self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now() + self->rpc_.retry_interval;

    self->owner_->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());
    return 0;
  }

  // 重试是模式是为了触发一下可能需要的token替换
  if (self->rpc_.is_retry_mode) {
    self->rpc_.is_retry_mode = false;
    // reset request time to invoke watch request immediately
    self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now();

    // 立刻开启下一次watch
    self->active();
    return 0;
  }

  std::string http_content;
  req.get_response_stream().str().swap(http_content);
  FWLOGTRACE("Etcd watcher {} got range http response: {}", reinterpret_cast<const void *>(self), http_content);

  rapidjson::Document doc;
  if (false == atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
    FWLOGERROR("Etcd watcher {} got range response parse failed: {}", reinterpret_cast<const void *>(self),
               http_content);

    self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now() + self->rpc_.retry_interval;
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
    FWLOGERROR("Etcd watcher {} got range response without header", reinterpret_cast<const void *>(self));

    self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now() + self->rpc_.retry_interval;
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
  response.compact_revision = 0;
  {
    rapidjson::Document::ConstMemberIterator res = doc.FindMember("kvs");

    if (doc.MemberEnd() != res) {
      size_t reverse_sz = 0;
      etcd_packer::unpack_int(doc, "count", reverse_sz);
      if (0 == reverse_sz) {
        reverse_sz = 64;
      }
      response.events.reserve(reverse_sz);

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

  if (util::log::log_wrapper::check_level(WDTLOGGETCAT(util::log::log_wrapper::categorize_t::DEFAULT),
                                          util::log::log_wrapper::level_t::LOG_LW_DEBUG)) {
    FWLOGDEBUG("Etcd watcher {} got range response", reinterpret_cast<const void *>(self));
    for (size_t i = 0; i < response.events.size(); ++i) {
      etcd_key_value *kv = &response.events[i].kv;
      FWLOGDEBUG("    InitEvt => type: PUT, key: {}, value: {}", kv->key, kv->value);
    }
  }

  // trigger event
  if (self->evt_handle_) {
    self->evt_handle_(header, response);
  }

  // reset request time to invoke watch request immediately
  self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now();

  // 立刻开启下一次watch
  self->active();
  return 0;
}

int etcd_watcher::libcurl_callback_on_watch_completed(util::network::http_request &req) {
  etcd_watcher *self = reinterpret_cast<etcd_watcher *>(req.get_priv_data());
  if (NULL == self) {
    FWLOGERROR("Etcd watcher watch request shouldn't has request without private data");
    return 0;
  }
  util::network::http_request::ptr_t keep_rpc = self->rpc_.rpc_opr_;
  self->rpc_.rpc_opr_.reset();
  self->rpc_.is_retry_mode = true;

  // 服务器错误则过一段时间后重试
  if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                       util::network::http_request::get_status_code_group(req.get_response_code())) {
    // timeout是正常的保活流程
    if (CURLE_OPERATION_TIMEDOUT != req.get_error_code()) {
      FWLOGERROR("Etcd watcher {} watch request failed, error code: {}, http code: {}\n{}",
                 reinterpret_cast<const void *>(self), req.get_error_code(), req.get_response_code(),
                 req.get_error_msg());

      self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now() + self->rpc_.retry_interval;

    } else {
      FWLOGDEBUG("Etcd watcher {} watch request finished, start another request later, msg: {}.",
                 reinterpret_cast<const void *>(self), req.get_error_msg());
      self->rpc_.watcher_next_request_time = util::time::time_utility::sys_now();
    }

    self->owner_->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());

    // 立刻开启下一次watch
    self->active();
    return 0;
  }

  FWLOGTRACE("Etcd watcher {} got watch http response", reinterpret_cast<const void *>(self));

  // 立刻开启下一次watch
  self->active();
  return 0;
}

int etcd_watcher::libcurl_callback_on_watch_write(util::network::http_request &req, const char *inbuf, size_t inbufsz,
                                                  const char *&outbuf, size_t &outbufsz) {
  // etcd_watcher 模块内消耗掉缓冲区，不需要写出到通用缓冲区了
  outbuf = NULL;
  outbufsz = 0;

  etcd_watcher *self = reinterpret_cast<etcd_watcher *>(req.get_priv_data());
  if (NULL == self) {
    FWLOGERROR("Etcd watcher watch request shouldn't has request without private data");
    return 0;
  }

  if (inbuf == NULL || 0 == inbufsz) {
    FWLOGDEBUG("Etcd watcher {} got http trunk without data", reinterpret_cast<const void *>(self));
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

    FWLOGTRACE("Etcd watcher {} got http trunk: {}", reinterpret_cast<const void *>(self), value_json);
    // 忽略空数据
    if (false == atapp::etcd_packer::parse_object(doc, value_json.c_str())) {
      continue;
    }
    if (!doc.IsObject()) {
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
        FWLOGERROR("Etcd watcher {} got http trunk without header", reinterpret_cast<const void *>(self));
      }
    }

    response_t response;
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
            FWLOGERROR("Etcd watcher {} got unknown event type. msg: {}", reinterpret_cast<const void *>(self),
                       value_json);
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

    if (util::log::log_wrapper::check_level(WDTLOGGETCAT(util::log::log_wrapper::categorize_t::DEFAULT),
                                            util::log::log_wrapper::level_t::LOG_LW_DEBUG)) {
      FWLOGDEBUG(
          "Etcd watcher {} got response: watch_id: {}, compact_revision: {}, created: {}, canceled: {}, event: {}",
          reinterpret_cast<const void *>(self), static_cast<long long>(response.watch_id),
          static_cast<long long>(response.compact_revision), response.created ? "Yes" : "No",
          response.canceled ? "Yes" : "No", static_cast<unsigned long long>(response.events.size()));
      for (size_t i = 0; i < response.events.size(); ++i) {
        etcd_key_value *kv = &response.events[i].kv;
        const char *name;
        if (etcd_watch_event::EN_WEVT_PUT == response.events[i].evt_type) {
          name = "PUT";
        } else {
          name = "DELETE";
        }
        FWLOGDEBUG("    Evt => type: {}, key: {}, value: {}", name, kv->key, kv->value);
      }
    }

    // trigger event
    if (self->evt_handle_) {
      self->evt_handle_(header, response);
    }

    // stopped if canceled and wait to start another watcher later
    if (response.canceled) {
      req.stop();
    }
  }

  return 0;
}

}  // namespace atapp
