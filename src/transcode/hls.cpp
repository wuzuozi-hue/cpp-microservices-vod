#include "vod/transcode/hls.h"
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace vod::transcode {
bool to_hls(const std::filesystem::path& input, const std::filesystem::path& output_dir,
            std::chrono::seconds timeout, std::string& error) {
    std::error_code ec;
    if (timeout.count() <= 0 || !std::filesystem::is_regular_file(input, ec) || ec ||
        !std::filesystem::is_directory(output_dir, ec) || ec || !std::filesystem::is_empty(output_dir, ec) || ec) {
        error = "input must be a file and output directory must exist and be empty"; return false;
    }
    const auto segment_pattern = (output_dir / "segment%05d.ts").string();
    const auto playlist = (output_dir / "index.m3u8").string();
    const std::vector<std::string> args = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
        "-i", input.string(), "-map", "0:v:0", "-map", "0:a:0?", "-c:v", "libx264", "-preset", "veryfast",
        "-threads", "1", "-pix_fmt", "yuv420p", "-force_key_frames", "expr:gte(t,n_forced*2)",
        "-c:a", "aac", "-f", "hls", "-hls_time", "2", "-hls_playlist_type", "vod",
        "-hls_segment_filename", segment_pattern, playlist};
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) { error = "cannot start ffmpeg"; return false; }
    if (child == 0) { setpgid(0, 0); execvp(argv[0], argv.data()); _exit(127); }
    setpgid(child, child);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    for (;;) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) { error = "cannot wait for ffmpeg"; return false; }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (kill(-child, SIGKILL) != 0) kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            error = "ffmpeg timed out"; return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !std::filesystem::is_regular_file(playlist, ec) || ec) {
        error = "ffmpeg failed to produce an HLS playlist"; return false;
    }
    error.clear(); return true;
}
}
