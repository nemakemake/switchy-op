#pragma once
#include <switch.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include "video.hpp"

// Framebuffer and console never own the display simultaneously. The connection manager
// keeps using the text console; this view owns the display only during a robot session.
class VideoScreen {
public:
    VideoScreen() {
        font_ = consoleGetDefault()->font;
        consoleExit(nullptr);
        ready_ = R_SUCCEEDED(framebufferCreate(&fb_, nwindowGetDefault(), 1280, 720,
                                              PIXEL_FORMAT_RGBA_8888, 2));
        if (ready_ && R_FAILED(framebufferMakeLinear(&fb_))) {
            framebufferClose(&fb_); ready_ = false;
        }
        if (!ready_) consoleInit(nullptr);
        hidInitializeTouchScreen();
    }
    ~VideoScreen() {
        if (ready_) { framebufferClose(&fb_); consoleInit(nullptr); }
    }
    bool ready() const { return ready_; }
    void draw(const sw::VideoFrame& frame, const std::string& title,
              const std::vector<std::string>& left, const std::vector<std::string>& right,
              bool estop, bool blocked) {
        if (!ready_) return;
        HidTouchScreenState touch{};
        hidGetTouchScreenStates(&touch, 1);
        bool down = touch.count > 0;
        if (down && !touching_ && touch.touches[0].y > 670) {
            if (touch.touches[0].x < 640) graphs_ = !graphs_;
            else full_ = !full_;
        }
        touching_ = down;
        double now = sw::video_now();
        bool live = sw::video_is_live(frame, now);
        if (now >= next_sample_) {
            fps_.push_back(live ? frame.fps : 0);
            gaps_.push_back(live ? frame.gap_ms : 1000);
            if (fps_.size() > 240) { fps_.pop_front(); gaps_.pop_front(); }
            next_sample_ = now + .25;
        }
        u32 stride;
        pixels_ = static_cast<u32*>(framebufferBegin(&fb_, &stride));
        stride_ = stride / 4;
        rect(0, 0, 1280, 720, RGBA8_MAXALPHA(14, 20, 28));
        text(24, 12, title, white);
        int area_height = full_ ? 610 : 430;
        if (!frame.rgba.empty()) {
            double scale = std::min(1232.0 / frame.width, double(area_height) / frame.height);
            int w = int(frame.width * scale), h = int(frame.height * scale);
            int x0 = (1280 - w) / 2, y0 = 44 + (area_height - h) / 2;
            for (int y = 0; y < h; ++y) {
                const auto* row = frame.rgba.data() + int(y / scale) * frame.width * 4;
                for (int x = 0; x < w; ++x) {
                    u32 color;
                    std::memcpy(&color, row + int(x / scale) * 4, 4);
                    pixels_[(y0 + y) * stride_ + x0 + x] = color;
                }
            }
        }
        // A stale frame is never presented with an apparently live status.
        std::string label = live ? "LIVE" : frame.received_at ? "VIDEO LOST" : "NO VIDEO";
        if (!live && frame.received_at)
            label += " | last decoded " + std::to_string(int(std::max(0.0, now - frame.received_at))) + "s ago";
        rect(24, 48, 1232, 24, RGBA8_MAXALPHA(14, 20, 28));
        text(32, 52, label + " | " + frame.status, live ? green : red);
        if (estop || blocked) {
            rect(24, 80, 1232, 24, RGBA8_MAXALPHA(100, 15, 20));
            text(32, 84, estop ? "EMERGENCY STOP ACTIVE" : "DRIVE BLOCKED: waiting for live video / release deadman", white);
        }
        if (!full_) {
            rect(24, 486, 606, 172, RGBA8_MAXALPHA(24, 33, 44));
            rect(650, 486, 606, 172, RGBA8_MAXALPHA(24, 33, 44));
            if (graphs_) {
                text(36, 494, "Decoded FPS | 60 seconds | 0..30", white);
                text(662, 494, "Frame interval ms | 0..1000", white);
                graph(fps_, 36, 526, 580, 116, 30);
                graph(gaps_, 662, 526, 580, 116, 1000);
            } else {
                for (size_t i = 0; i < left.size() && i < 7; ++i) text(36, 494 + i * 22, left[i].substr(0, 36), white);
                for (size_t i = 0; i < right.size() && i < 7; ++i) text(662, 494 + i * 22, right[i].substr(0, 36), white);
            }
        }
        text(24, 682, "[Touch: graphs]", green);
        text(664, 682, "[Touch: fullscreen]", green);
        framebufferEnd(&fb_);
    }
private:
    void rect(int x, int y, int w, int h, u32 color) {
        for (int row = y; row < std::min(y + h, 720); ++row)
            std::fill_n(pixels_ + row * stride_ + x, std::min(w, 1280 - x), color);
    }
    void text(int x, int y, const std::string& value, u32 color) {
        const auto* font = static_cast<const u16*>(font_.gfx);
        for (unsigned char c : value) {
            if (x + 16 > 1280) break;
            if (c >= font_.asciiOffset && c < font_.asciiOffset + font_.numChars)
                for (int row = 0; row < 16 && y + row < 720; ++row)
                    for (int col = 0; col < 16; ++col)
                        if (font[(c - font_.asciiOffset) * 16 + row] & (0x8000 >> col))
                            pixels_[(y + row) * stride_ + x + col] = color;
            x += 16;
        }
    }
    void graph(const std::deque<double>& samples, int x, int y, int w, int h, double maximum) {
        rect(x, y + h, w, 1, white);
        for (size_t i = 0; i < samples.size(); ++i) {
            int height = int(std::clamp(samples[i] / maximum, 0.0, 1.0) * h);
            rect(x + int(i * w / 240), y + h - height, 2, std::max(1, height), green);
        }
    }
    static constexpr u32 white = RGBA8_MAXALPHA(222, 232, 242);
    static constexpr u32 green = RGBA8_MAXALPHA(64, 220, 170);
    static constexpr u32 red = RGBA8_MAXALPHA(255, 100, 100);
    Framebuffer fb_{};
    ConsoleFont font_{};
    bool ready_ = false, touching_ = false, graphs_ = false, full_ = false;
    u32* pixels_ = nullptr;
    u32 stride_ = 0;
    double next_sample_ = 0;
    std::deque<double> fps_, gaps_;
};
