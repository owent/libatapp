#include <libatbus.h>

#include <algorithm/base64.h>

#include <log/log_wrapper.h>

#include <atframe/etcdcli/etcd_cluster.h>
#include <atframe/etcdcli/etcd_keepalive.h>


namespace atapp {

    etcd_keepalive::default_checker_t::default_checker_t(const std::string &checked) : data(checked) {}
    etcd_keepalive::default_checker_t::~default_checker_t() {}

    bool etcd_keepalive::default_checker_t::operator()(const std::string &checked) const { return checked.empty() || data == checked; }

    LIBATAPP_MACRO_API etcd_keepalive::etcd_keepalive(etcd_cluster &owner, const std::string &path, constrict_helper_t &)
        : owner_(&owner), path_(path) {
        checker_.is_check_run    = false;
        checker_.is_check_passed = false;
        checker_.retry_times     = 0;
        rpc_.is_actived          = false;
        rpc_.is_value_changed    = true;
        rpc_.has_data            = false;
    }

    LIBATAPP_MACRO_API etcd_keepalive::~etcd_keepalive() { close(true); }

    LIBATAPP_MACRO_API etcd_keepalive::ptr_t etcd_keepalive::create(etcd_cluster &owner, const std::string &path) {
        constrict_helper_t h;
        return std::make_shared<etcd_keepalive>(owner, path, h);
    }

    LIBATAPP_MACRO_API void etcd_keepalive::close(bool reset_has_data_flag) {
        if (rpc_.rpc_opr_) {
            FWLOGDEBUG("Etcd watcher {} cancel http request.", reinterpret_cast<const void *>(this));
            rpc_.rpc_opr_->set_on_complete(NULL);
            rpc_.rpc_opr_->set_priv_data(NULL);
            rpc_.rpc_opr_->stop();
            rpc_.rpc_opr_.reset();
        }

        rpc_.is_actived       = false;
        rpc_.is_value_changed = true;
        if (reset_has_data_flag) {
            rpc_.has_data = false;
        }

        checker_.is_check_run    = false;
        checker_.is_check_passed = false;
        checker_.fn              = NULL;
        checker_.retry_times     = 0;
    }

    LIBATAPP_MACRO_API void etcd_keepalive::set_checker(const std::string &checked_str) { checker_.fn = default_checker_t(checked_str); }

    LIBATAPP_MACRO_API void etcd_keepalive::set_checker(checker_fn_t fn) { checker_.fn = fn; }

    LIBATAPP_MACRO_API void etcd_keepalive::set_value(const std::string &str) {
        if (value_ != str) {
            value_                = str;
            rpc_.is_value_changed = true;

            if (NULL != owner_ && owner_->check_flag(etcd_cluster::flag_t::RUNNING) && 0 != owner_->get_lease()) {
                active();
            }
        }
    }

    LIBATAPP_MACRO_API void etcd_keepalive::reset_value_changed() { rpc_.is_value_changed = true; }

    LIBATAPP_MACRO_API const std::string &etcd_keepalive::get_value() const { return value_; }

    LIBATAPP_MACRO_API const std::string &etcd_keepalive::get_path() const { return path_; }

    LIBATAPP_MACRO_API void etcd_keepalive::active() {
        rpc_.is_actived = true;
        process();
    }

    void etcd_keepalive::process() {
        if (rpc_.rpc_opr_) {
            return;
        }

        rpc_.is_actived = false;

        // if has checker and has not check date yet, send a check request
        if (!checker_.fn) {
            checker_.is_check_run    = true;
            checker_.is_check_passed = true;
            ++checker_.retry_times;
        }

        bool need_retry = false;
        do {
            if (false == checker_.is_check_run) {
                // create a check rpc
                rpc_.rpc_opr_ = owner_->create_request_kv_get(path_);
                if (!rpc_.rpc_opr_) {
                    need_retry = true;
                    ++checker_.retry_times;
                    FWLOGERROR("Etcd keepalive {} create get data request to {} failed", reinterpret_cast<const void *>(this), path_);
                    break;
                }

                rpc_.rpc_opr_->set_priv_data(this);
                rpc_.rpc_opr_->set_on_complete(libcurl_callback_on_get_data);
                break;
            }

            // if check passed, set data
            if (checker_.is_check_run && checker_.is_check_passed && rpc_.is_value_changed) {
                // create set data rpc
                rpc_.rpc_opr_ = owner_->create_request_kv_set(path_, value_, true);
                if (!rpc_.rpc_opr_) {
                    need_retry = true;
                    FWLOGERROR("Etcd keepalive {} create set data request to {} failed", reinterpret_cast<const void *>(this), path_);
                    break;
                }

                rpc_.is_value_changed = false;
                rpc_.rpc_opr_->set_priv_data(this);
                rpc_.rpc_opr_->set_on_complete(libcurl_callback_on_set_data);
            }
        } while (false);

        if (rpc_.rpc_opr_) {
            int res = rpc_.rpc_opr_->start(util::network::http_request::method_t::EN_MT_POST, false);
            if (res != 0) {
                need_retry = true;
                rpc_.rpc_opr_->set_priv_data(NULL);
                rpc_.rpc_opr_->set_on_complete(NULL);
                FWLOGERROR("Etcd keepalive {} start request to {} failed, res: {}", reinterpret_cast<const void *>(this),
                           rpc_.rpc_opr_->get_url(), res);
                rpc_.rpc_opr_.reset();
            } else {
                FWLOGDEBUG("Etcd keepalive {} start request to {} success.", reinterpret_cast<const void *>(this),
                           rpc_.rpc_opr_->get_url());
            }
        }

        if (need_retry) {
            owner_->add_retry_keepalive(shared_from_this());
        }
    }

    int etcd_keepalive::libcurl_callback_on_get_data(util::network::http_request &req) {
        etcd_keepalive *self = reinterpret_cast<etcd_keepalive *>(req.get_priv_data());
        if (NULL == self) {
            FWLOGERROR("Etcd keepalive get request shouldn't has request without private data");
            return 0;
        }
        util::network::http_request::ptr_t keep_rpc = self->rpc_.rpc_opr_;

        self->rpc_.rpc_opr_.reset();
        ++self->checker_.retry_times;

        // 服务器错误则重试，预检查请求的404是正常的
        if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                             util::network::http_request::get_status_code_group(req.get_response_code())) {
            FWLOGERROR("Etcd keepalive {} get request failed, error code: {}, http code: {}\n{}", reinterpret_cast<const void *>(self),
                       req.get_error_code(), req.get_response_code(), req.get_error_msg());

            self->owner_->add_retry_keepalive(self->shared_from_this());
            self->owner_->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());
            return 0;
        }

        std::string http_content;
        std::string value_content;

        req.get_response_stream().str().swap(http_content);
        FWLOGTRACE("Etcd keepalive {} got http response: {}", reinterpret_cast<const void *>(self), http_content);

        // 如果lease不存在（没有TTL）则启动创建流程
        rapidjson::Document doc;

        if (atapp::etcd_packer::parse_object(doc, http_content.c_str())) {
            rapidjson::Value& root = doc;

            // Run check function
            int64_t count = 0;

            etcd_packer::unpack_int(root, "count", count);
            if (count > 0) {
                rapidjson::Document::ConstMemberIterator kvs = root.FindMember("kvs");
                if (root.MemberEnd() == kvs) {
                    FWLOGERROR("Etcd keepalive {} get data count={}, but kvs not found", reinterpret_cast<const void *>(self), count);
                    self->owner_->add_retry_keepalive(self->shared_from_this());
                    return 0;
                }

                if (!kvs->value.IsArray()) {
                    FWLOGERROR("Etcd keepalive {} get data count={}, but kvs is not array", reinterpret_cast<const void *>(self), count);
                    self->owner_->add_retry_keepalive(self->shared_from_this());
                    return 0;
                }

                rapidjson::Document::ConstArray all_kvs = kvs->value.GetArray();
                if (all_kvs.Begin() != all_kvs.End()) {
                    etcd_key_value kv;
                    etcd_packer::unpack(kv, *all_kvs.Begin());
                    value_content.swap(kv.value);
                }
            }
        }

        self->checker_.is_check_run = true;
        if (!self->checker_.fn) {
            self->checker_.is_check_passed = true;
        } else {
            self->checker_.is_check_passed = self->checker_.fn(value_content);
        }
        FWLOGDEBUG("Etcd keepalive {} check data {}", reinterpret_cast<const void *>(self),
                   self->checker_.is_check_passed ? "passed" : "failed");

        self->active();
        return 0;
    }

    int etcd_keepalive::libcurl_callback_on_set_data(util::network::http_request &req) {
        etcd_keepalive *self = reinterpret_cast<etcd_keepalive *>(req.get_priv_data());
        if (NULL == self) {
            FWLOGERROR("Etcd keepalive set request shouldn't has request without private data");
            return 0;
        }

        util::network::http_request::ptr_t keep_rpc = self->rpc_.rpc_opr_;
        self->rpc_.rpc_opr_.reset();

        // 服务器错误则忽略
        if (0 != req.get_error_code() || util::network::http_request::status_code_t::EN_ECG_SUCCESS !=
                                             util::network::http_request::get_status_code_group(req.get_response_code())) {
            FWLOGERROR("Etcd keepalive {} set request failed, error code: {}, http code: {}\n{}", reinterpret_cast<const void *>(self),
                       req.get_error_code(), req.get_response_code(), req.get_error_msg());

            self->rpc_.is_value_changed = true;
            self->owner_->add_retry_keepalive(self->shared_from_this());
            self->owner_->check_authorization_expired(req.get_response_code(), req.get_response_stream().str());
            return 0;
        }

        self->rpc_.has_data = true;
        FWLOGDEBUG("Etcd keepalive {} set data http response: {}", reinterpret_cast<const void *>(self), req.get_response_stream().str());
        self->active();
        return 0;
    }
} // namespace atapp
