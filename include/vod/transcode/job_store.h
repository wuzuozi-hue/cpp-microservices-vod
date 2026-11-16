#pragma once
#include "vod/user/session_store.h"
#include <optional>
#include <string>

namespace vod::transcode {
struct Job { std::string video_id, source_file_id, owner_id, claim_id; };
class JobStore {
public:
    explicit JobStore(user::DatabaseConfig config);
    // A ten-minute claim allows a crashed worker's job to be retried.
    std::optional<Job> claim() const;
    void add_artifact(const Job& job, const std::string& file_id) const;
    bool complete(const Job& job, const std::string& playlist_id) const;
    void fail(const Job& job) const;
private:
    user::DatabaseConfig config_;
};
}
