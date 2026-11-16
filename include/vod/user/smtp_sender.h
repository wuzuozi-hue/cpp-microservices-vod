#pragma once
#include <string>

namespace vod::user {
struct SmtpConfig {
    std::string url, username, password, from, ca_file, login_options;
};
class SmtpSender {
public:
    explicit SmtpSender(SmtpConfig config);
    void operator()(const std::string& recipient, const std::string& code) const;
private:
    SmtpConfig config_;
};
}
