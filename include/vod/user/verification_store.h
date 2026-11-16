#pragma once
#include "vod/user/session_store.h"
#include <functional>
#include <string>

namespace vod::user {
using CodeSender = std::function<void(const std::string& email, const std::string& code)>;
class VerificationStore {
public:
    VerificationStore(DatabaseConfig config, std::string secret, CodeSender sender);
    // Throws on invalid input, invalid session, database failure or delivery failure.
    std::string issue(const std::string& session_id, const std::string& email) const;
    // Returns false for invalid, expired, consumed or incorrect codes.
    bool consume(const std::string& session_id, const std::string& email,
                 const std::string& code_id, const std::string& code) const;
private:
    DatabaseConfig config_;
    std::string secret_;
    CodeSender sender_;
};
}
