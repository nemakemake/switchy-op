// Safety policy tests independent of FFmpeg, sockets and the Switch display.
#include "../core/video.hpp"
#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool value, const char* name) {
        std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
        if (!value) ++failures;
    };
    sw::VideoFrame frame;
    check(!sw::video_is_live(frame, 10), "no frame is never live");
    frame.received_at = 10; frame.status = "LIVE";
    check(sw::video_is_live(frame, 10.9), "recent decoded frame");
    check(!sw::video_is_live(frame, 11), "one second without a frame is stale");
    check(!sw::video_is_live(frame, 9), "future timestamps rejected");
    frame.status = "video interrupted; retrying";
    check(!sw::video_is_live(frame, 10.1), "read error immediately invalidates video");
    sw::VideoDriveGuard guard;
    check(guard.update(true, true, true), "held deadman at startup cannot arm");
    check(!guard.update(true, true, false), "release with live video arms");
    check(!guard.update(true, true, true), "fresh press permits driving");
    check(guard.update(true, false, true), "video loss stops driving");
    check(guard.update(true, true, true), "video recovery cannot restart held drive");
    check(guard.update(true, false, false), "release during outage does not arm");
    check(guard.update(true, true, true), "press after release during outage stays blocked");
    check(!guard.update(true, true, false), "release after recovery rearms");
    check(!guard.update(true, true, true), "new press after recovery drives");
    check(!guard.update(false, false, true), "explicit video opt-out permits camera-free control");
    check(guard.update(true, true, true), "reenabling interlock requires release");
    return failures ? 1 : 0;
}
