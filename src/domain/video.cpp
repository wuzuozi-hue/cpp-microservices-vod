#include "vod/domain/video.h"

#include <limits>

namespace vod {
std::optional<std::size_t> utf8_length(std::string_view value) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size(); ++count) {
        const auto first = static_cast<unsigned char>(value[i]);
        std::uint32_t code = 0;
        std::size_t bytes = 0;
        if (first < 0x80) { code = first; bytes = 1; }
        else if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; bytes = 2; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; bytes = 3; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; bytes = 4; }
        else return std::nullopt;
        if (bytes > value.size() - i) return std::nullopt;
        for (std::size_t j = 1; j < bytes; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if ((next & 0xc0) != 0x80) return std::nullopt;
            code = (code << 6) | (next & 0x3f);
        }
        if ((bytes == 3 && code < 0x800) || (bytes == 4 && code < 0x10000) ||
            (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff)
            return std::nullopt;
        i += bytes;
    }
    return count;
}

bool valid_title(std::string_view value) {
    const auto length = utf8_length(value);
    return length && *length >= 1 && *length <= 80;
}

bool valid_description(std::string_view value) {
    const auto length = utf8_length(value);
    return length && *length <= 500;
}

bool publicly_visible(VideoStatus status) { return status == VideoStatus::published; }

std::optional<VideoStatus> transition(VideoStatus current, VideoAction action) {
    using S = VideoStatus;
    using A = VideoAction;
    if (current == S::processing && action == A::processing_succeeded) return S::pending_review;
    if (current == S::processing && action == A::processing_failed) return S::processing_failed;
    if (current == S::pending_review && action == A::approve) return S::published;
    if (current == S::pending_review && action == A::reject) return S::rejected;
    if (current == S::published && action == A::take_offline) return S::offline;
    if ((current == S::offline || current == S::rejected) && action == A::request_review)
        return S::pending_review;
    if (current == S::processing_failed && action == A::retry_processing) return S::processing;
    return std::nullopt;
}

std::optional<std::uint64_t> page_offset(std::uint64_t page, std::uint64_t page_size) {
    if (!page || !page_size || page - 1 > std::numeric_limits<std::uint64_t>::max() / page_size)
        return std::nullopt;
    return (page - 1) * page_size;
}
} // namespace vod
