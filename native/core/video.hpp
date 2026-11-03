#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sw {
// A single decoded frame, replaced rather than queued. Times use the local monotonic clock;
// received_at is NOT the exposure time on the camera.
struct VideoFrame {
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    double received_at = 0, fps = 0, gap_ms = 0;
    unsigned long sequence = 0, reconnects = 0;
    std::string status = "connecting";
};

double video_now();

inline bool video_is_live(const VideoFrame& frame, double now) {
    return frame.received_at > 0 && now >= frame.received_at &&
           now - frame.received_at < 1.0 && frame.status == "LIVE";
}

// Losing video latches the interlock. A held deadman must never restart the robot
// when a new frame arrives; it must first be released with live video available.
class VideoDriveGuard {
public:
    bool update(bool required, bool live, bool deadman) {
        if (!required) { armed_ = false; return false; }
        if (!live) armed_ = false;
        else if (!deadman) armed_ = true;
        return !armed_;
    }
    bool blocked(bool required) const { return required && !armed_; }
private:
    bool armed_ = false;
};

class VideoReceiver {
public:
    explicit VideoReceiver(std::string url);
    ~VideoReceiver();
    VideoReceiver(const VideoReceiver&) = delete;
    VideoReceiver& operator=(const VideoReceiver&) = delete;
    // Copies pixels only when a new frame arrives, keeping render work bounded.
    void snapshot(VideoFrame& destination);
private:
    static int interrupted(void* opaque);
    void run();
    void session();
    void status(const std::string& value);
    std::string url_;
    std::atomic<bool> stopping_{false};
    std::atomic<double> deadline_{0};
    std::mutex mutex_;
    VideoFrame latest_;
    std::thread worker_;
};
} // namespace sw
