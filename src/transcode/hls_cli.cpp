#include "vod/transcode/hls.h"
#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: vod_hls INPUT EMPTY_OUTPUT_DIRECTORY\n"; return 2; }
    std::string error;
    if (!vod::transcode::to_hls(argv[1], argv[2], std::chrono::minutes(5), error)) { std::cerr << error << '\n'; return 1; }
    std::cout << "HLS_TRANSCODE_OK\n";
}
