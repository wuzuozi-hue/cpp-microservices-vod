#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace vod::user {
struct DatabaseConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database;
};

struct Session { std::string id; bool guest; std::string user_id; int role_type = 4; };

// Each operation has its own connection, so calls may run concurrently. The
// database must already contain the schema from sql/001_sessions.sql.
class SessionStore {
public:
    explicit SessionStore(DatabaseConfig config);
    Session create_guest() const;
    std::optional<Session> find(const std::string& id) const;
    bool remove(const std::string& id) const;
private:
    DatabaseConfig config_;
};
}
