#pragma once
#include "vod/user/session_store.h"
#include <optional>
#include <string>
#include <vector>

namespace vod::user {
struct Profile { std::string user_id, email, nickname; int role_type = 0; std::string avatar_id; };
struct Administrator { std::string user_id, email, nickname, memo; bool enabled = false; };
struct AdministratorPage { std::uint64_t total = 0; std::vector<Administrator> items; };
class AccountStore {
public:
    explicit AccountStore(DatabaseConfig config);
    std::optional<Profile> profile(const std::string& session_id) const;
    std::optional<Profile> public_profile(const std::string& user_id) const;
    bool set_nickname(const std::string& session_id, const std::string& nickname) const;
    bool set_avatar(const std::string& session_id, const std::string& file_id) const;
    bool set_password(const std::string& session_id, const std::string& password) const;
    bool password_login(const std::string& session_id, const std::string& username, const std::string& password) const;
    bool set_follow(const std::string& session_id, const std::string& target_id, bool follow) const;
    bool manage_admin(const std::string& session_id, const std::string& action, const std::string& target,
                      const std::string& nickname, const std::string& memo, bool enabled) const;
    AdministratorPage list_admins(const std::string& session_id, const std::string& email, int status,
                                  std::uint64_t page, std::uint64_t page_size) const;
private:
    DatabaseConfig config_;
};
}
