#pragma once
#include "vod/domain/video.h"
#include "vod/user/session_store.h"
#include <optional>
#include <string>
#include <vector>

namespace vod::video {
struct Video {
    std::string id, owner_id, title, description;
    VideoStatus status = VideoStatus::processing;
    int video_type = 0, duration_seconds = 0;
    std::vector<int> tags;
};
struct VideoSummary { std::string id, owner_id, title, description, playlist_file_id, cover_file_id; int status = 0; std::uint64_t likes = 0, plays = 0; };
struct VideoPage { std::uint64_t total = 0; std::vector<VideoSummary> items; };
struct Barrage { std::string id, user_id, content; std::uint32_t seconds = 0; };
struct Interaction { bool liked = false; std::uint64_t likes = 0, plays = 0; std::vector<Barrage> barrage; };
class VideoStore {
public:
    explicit VideoStore(user::DatabaseConfig config);
    void create(const Video& video) const;
    // Validates file ownership and creates video plus pending job atomically.
    void create_with_source(const Video& video, const std::string& source_file_id,
                            const std::string& cover_file_id = "") const;
    std::optional<VideoStatus> status(const std::string& id) const;
    // Optimistic update: false means the expected state was no longer current.
    bool apply(const std::string& id, VideoStatus expected, VideoAction action) const;
    bool review(const std::string& id, const std::string& reviewer_id, bool approved) const;
    bool change_visibility(const std::string& id, const std::string& actor_id, bool request_review) const;
    bool remove_video(const std::string& id, const std::string& actor_id) const;
    VideoPage list_public(std::uint64_t page, std::uint64_t page_size, const std::string& search = "", int type = -1, int tag = -1) const;
    VideoPage list_for_review(const std::string& reviewer_id, int status, std::uint64_t page, std::uint64_t page_size) const;
    VideoPage list_for_owner(const std::string& owner_id, std::uint64_t page, std::uint64_t page_size) const;
    Interaction interact(const std::string& video_id, const std::string& user_id, const std::string& action,
                         const std::string& barrage_id = "", const std::string& content = "", std::uint32_t seconds = 0) const;
private:
    user::DatabaseConfig config_;
};
}
