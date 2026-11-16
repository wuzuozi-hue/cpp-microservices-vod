#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vod {

// Numeric values are part of the course HTTP contract.
enum class VideoStatus : std::int32_t {
    pending_review = 1,
    published = 2,
    rejected = 3,
    offline = 4,
    processing = 5,
    processing_failed = 6,
    deleted = 7,
};

enum class VideoAction { processing_succeeded, processing_failed, approve,
                         reject, take_offline, request_review, retry_processing };

// Return the number of Unicode scalar values, or nullopt for invalid UTF-8.
std::optional<std::size_t> utf8_length(std::string_view value);
bool valid_title(std::string_view value);
bool valid_description(std::string_view value);
bool publicly_visible(VideoStatus status);
// Invalid transitions are rejected and leave the caller's state unchanged.
std::optional<VideoStatus> transition(VideoStatus current, VideoAction action);

// HTTP pages start at 1; storage offsets start at 0. Reject invalid or
// overflowing requests instead of wrapping unsigned arithmetic.
std::optional<std::uint64_t> page_offset(std::uint64_t page,
                                         std::uint64_t page_size);

} // namespace vod
