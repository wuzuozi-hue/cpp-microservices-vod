#include "vod/domain/video.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

static void check(bool value, const char* description) {
    if (!value) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}

int main() {
    check(vod::utf8_length(u8"视频😀") == 3, "Unicode count");
    check(!vod::utf8_length("\xc0\x80"), "overlong UTF-8");
    check(!vod::utf8_length("\xed\xa0\x80"), "surrogate UTF-8");
    check(!vod::utf8_length("\xf4\x90\x80\x80"), "out of range UTF-8");
    check(!vod::utf8_length("\xe4\xb8"), "truncated UTF-8");
    check(!vod::valid_title("") && vod::valid_title(std::string(80, 'x')) &&
          !vod::valid_title(std::string(81, 'x')), "title boundaries");
    check(vod::valid_description("") && vod::valid_description(std::string(500, 'x')) &&
          !vod::valid_description(std::string(501, 'x')), "description boundaries");
    check(vod::publicly_visible(vod::VideoStatus::published) &&
          !vod::publicly_visible(vod::VideoStatus::pending_review) &&
          !vod::publicly_visible(vod::VideoStatus::offline), "visibility");
    using S = vod::VideoStatus;
    using A = vod::VideoAction;
    check(vod::transition(S::processing, A::processing_succeeded) == S::pending_review &&
          vod::transition(S::processing, A::processing_failed) == S::processing_failed &&
          vod::transition(S::pending_review, A::approve) == S::published &&
          vod::transition(S::pending_review, A::reject) == S::rejected &&
          vod::transition(S::published, A::take_offline) == S::offline &&
          vod::transition(S::offline, A::request_review) == S::pending_review &&
          vod::transition(S::processing_failed, A::retry_processing) == S::processing,
          "video lifecycle");
    check(!vod::transition(S::processing, A::approve) &&
          !vod::transition(S::rejected, A::approve) &&
          !vod::transition(S::offline, A::approve), "invalid transitions rejected");
    check(vod::page_offset(1, 20) == 0 && vod::page_offset(2, 20) == 20 &&
          !vod::page_offset(0, 20) && !vod::page_offset(1, 0) &&
          !vod::page_offset(std::numeric_limits<std::uint64_t>::max(), 20), "paging");
    std::cout << "video domain tests passed\n";
}
