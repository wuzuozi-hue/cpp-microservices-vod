#include "bite_scaffold/fdfs.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        bitefdfs::fdfs_settings settings;
        settings.trackers.push_back(argv[1]);
        settings.connect_timeout = 3;
        settings.network_timeout = 5;
        bitefdfs::FDFSClient::init(settings);
        struct Cleanup { ~Cleanup() { bitefdfs::FDFSClient::destroy(); } } cleanup;
        const std::string input("vod-test\0binary\n", 16);
        auto id = bitefdfs::FDFSClient::upload_from_buff(input);
        if (!id) { std::cerr << "FastDFS upload failed\n"; return 1; }
        std::string output;
        const bool downloaded = bitefdfs::FDFSClient::download_to_buff(*id, output);
        const bool deleted = bitefdfs::FDFSClient::remove(*id);
        if (!downloaded || output != input || !deleted) { std::cerr << "FastDFS roundtrip or cleanup failed\n"; return 1; }
        if (bitefdfs::FDFSClient::download_to_buff(*id, output)) { std::cerr << "deleted file remained accessible\n"; return 1; }
        std::cout << "FastDFS roundtrip passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
