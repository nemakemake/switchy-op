#include "video.hpp"
#ifdef SWITCHY_VIDEO

#include <algorithm>
#include <chrono>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace sw {
double video_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
VideoReceiver::VideoReceiver(std::string url) : url_(std::move(url)), worker_([this] { run(); }) {}
VideoReceiver::~VideoReceiver() {
    stopping_ = true;
    if (worker_.joinable()) worker_.join();
}
int VideoReceiver::interrupted(void* opaque) {
    auto& self = *static_cast<VideoReceiver*>(opaque);
    return self.stopping_ || video_now() > self.deadline_;
}
void VideoReceiver::snapshot(VideoFrame& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out.sequence != latest_.sequence) out = latest_;
    else { out.status = latest_.status; out.reconnects = latest_.reconnects; }
}
void VideoReceiver::status(const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.status = value;
}
void VideoReceiver::run() {
    // stderr may be backed by the text console, which is closed while the GUI owns
    // the framebuffer. Report failures through status() instead of that renderer.
    av_log_set_level(AV_LOG_QUIET);
    avformat_network_init();
    while (!stopping_) {
        session();
        if (stopping_) break;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++latest_.reconnects;
        }
        for (int i = 0; i < 20 && !stopping_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    avformat_network_deinit();
}
void VideoReceiver::session() {
    AVFormatContext* format = avformat_alloc_context();
    if (!format) { status("out of memory"); return; }
    format->interrupt_callback = {interrupted, this};
    format->flags |= AVFMT_FLAG_NOBUFFER;
    AVDictionary* options = nullptr;
    // UDP prevents a missing video packet from holding up all subsequent frames.
    av_dict_set(&options, "rtsp_transport", "udp", 0);
    av_dict_set(&options, "protocol_whitelist", "rtsp,rtp,udp,tcp", 0);
    av_dict_set(&options, "max_delay", "100000", 0);
    av_dict_set(&options, "probesize", "32768", 0);
    av_dict_set(&options, "analyzeduration", "1000000", 0);
    deadline_ = video_now() + 5;
    int opened = avformat_open_input(&format, url_.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (opened < 0) { status("video unavailable; retrying"); avformat_close_input(&format); return; }
    deadline_ = video_now() + 5;
    int stream = avformat_find_stream_info(format, nullptr) < 0 ? -1
        : av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* scaler = nullptr;
    bool ready = false;
    if (stream >= 0 && format->streams[stream]->codecpar->codec_id == AV_CODEC_ID_H264) {
        decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
        if (decoder && avcodec_parameters_to_context(decoder, format->streams[stream]->codecpar) >= 0) {
            decoder->thread_count = 2;
            decoder->thread_type = FF_THREAD_SLICE;
            decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
            decoder->max_pixels = 1920 * 1080;
            ready = packet && frame && avcodec_open2(decoder, decoder->codec, nullptr) >= 0;
        }
    }
    status(ready ? "waiting for H.264 keyframe" : "H.264 decoder/stream unavailable");
    double previous = 0, window = video_now();
    int count = 0;
    double fps = 0;
    while (ready && !stopping_) {
        deadline_ = video_now() + 2;
        if (av_read_frame(format, packet) < 0) break;
        if (packet->stream_index == stream && avcodec_send_packet(decoder, packet) >= 0) {
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                if (frame->width < 1 || frame->height < 1 || frame->width > 1920 || frame->height > 1080) {
                    status("video exceeds 1920x1080 limit"); ready = false; break;
                }
                // Include pixel aspect ratio, and fit the software-decoded image in 640x360.
                AVRational sar = av_guess_sample_aspect_ratio(format, format->streams[stream], frame);
                double ratio = double(frame->width) / frame->height;
                if (sar.num > 0 && sar.den > 0) ratio *= av_q2d(sar);
                int w = std::clamp(int(std::min(640.0, 360.0 * ratio)), 1, 640);
                int h = std::clamp(int(w / ratio), 1, 360);
                scaler = sws_getCachedContext(scaler, frame->width, frame->height,
                    static_cast<AVPixelFormat>(frame->format), w, h, AV_PIX_FMT_RGBA,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!scaler) { ready = false; status("video conversion failed"); break; }
                VideoFrame next;
                next.width = w; next.height = h;
                next.rgba.resize(w * h * 4);
                uint8_t* pixels[] = {next.rgba.data(), nullptr, nullptr, nullptr};
                int strides[] = {w * 4, 0, 0, 0};
                sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, pixels, strides);
                double now = video_now();
                ++count;
                if (now - window >= 1) { fps = count / (now - window); window = now; count = 0; }
                next.received_at = now; next.fps = fps;
                next.gap_ms = previous ? (now - previous) * 1000 : 0;
                previous = now;
                next.status = "LIVE";
                std::lock_guard<std::mutex> lock(mutex_);
                next.sequence = latest_.sequence + 1;
                next.reconnects = latest_.reconnects;
                latest_ = std::move(next);
            }
        }
        av_packet_unref(packet);
        // An audio-only or undecodable stream must not suppress reconnect forever.
        if (video_now() - (previous ? previous : window) > 5) break;
    }
    if (ready) status("video interrupted; retrying");
    sws_freeContext(scaler);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
}
} // namespace sw
#endif
