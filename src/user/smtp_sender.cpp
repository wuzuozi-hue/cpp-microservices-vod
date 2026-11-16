#include "vod/user/smtp_sender.h"
#include <curl/curl.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace vod::user {
namespace {
struct CurlClose { void operator()(CURL* handle) const { if (handle) curl_easy_cleanup(handle); } };
struct ListClose { void operator()(curl_slist* list) const { curl_slist_free_all(list); } };
struct Payload { std::string body; std::size_t position = 0; };
size_t read_data(char* buffer, size_t size, size_t count, void* pointer) {
    auto& payload = *static_cast<Payload*>(pointer);
    if (!size || count > static_cast<std::size_t>(-1) / size) return 0;
    const auto amount = std::min(size * count, payload.body.size() - payload.position);
    std::memcpy(buffer, payload.body.data() + payload.position, amount);
    payload.position += amount;
    return amount;
}
bool safe_address(const std::string& value) {
    const auto at = value.find('@');
    return !value.empty() && value.size() <= 254 && at != std::string::npos && at > 0 && at + 1 < value.size() &&
        value.find('@', at + 1) == std::string::npos &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!#$%&'*+-/=?^_`{|}~@") == std::string::npos;
}
}
SmtpSender::SmtpSender(SmtpConfig config) : config_(std::move(config)) {
    if ((config_.url.rfind("smtp://", 0) != 0 && config_.url.rfind("smtps://", 0) != 0) ||
        config_.url.find_first_of("\r\n") != std::string::npos || !safe_address(config_.from) ||
        (!config_.login_options.empty() && config_.login_options != "AUTH=LOGIN"))
        throw std::invalid_argument("invalid SMTP URL or sender address");
    static std::once_flag initialized;
    std::call_once(initialized, [] { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl initialization failed"); });
}
void SmtpSender::operator()(const std::string& recipient, const std::string& code) const {
    if (!safe_address(recipient) || code.size() != 6 || code.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid recipient or verification code");
    std::unique_ptr<CURL, CurlClose> curl(curl_easy_init());
    if (!curl) throw std::runtime_error("cannot initialize SMTP client");
    std::unique_ptr<curl_slist, ListClose> recipients(curl_slist_append(nullptr, recipient.c_str()));
    if (!recipients) throw std::runtime_error("cannot allocate SMTP recipient");
    Payload payload{"From: <" + config_.from + ">\r\nTo: <" + recipient + ">\r\nSubject: Video platform verification code\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n\r\nVerification code: " + code + "\r\n"};
    curl_easy_setopt(curl.get(), CURLOPT_URL, config_.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!config_.ca_file.empty()) curl_easy_setopt(curl.get(), CURLOPT_CAINFO, config_.ca_file.c_str());
    if (!config_.username.empty()) curl_easy_setopt(curl.get(), CURLOPT_USERNAME, config_.username.c_str());
    if (!config_.password.empty()) curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, config_.password.c_str());
    if (!config_.login_options.empty()) curl_easy_setopt(curl.get(), CURLOPT_LOGIN_OPTIONS, config_.login_options.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_FROM, config_.from.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_RCPT, recipients.get());
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, read_data);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &payload);
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 20L);
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) throw std::runtime_error(std::string("SMTP delivery failed: ") + curl_easy_strerror(result));
}
}
