#pragma once
#include <chrono>
#include <filesystem>
#include <string>

namespace vod::transcode {
// Transcode the first video and optional first audio stream to H.264/AAC HLS.
// output_dir must be empty. The caller owns it and its cleanup.
bool to_hls(const std::filesystem::path& input, const std::filesystem::path& output_dir,
            std::chrono::seconds timeout, std::string& error);
}
