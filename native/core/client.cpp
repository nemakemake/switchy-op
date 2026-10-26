#include "client.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace sw {
namespace {

constexpr double kHandshakeTimeoutS = 5.0;

double steady_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

Client::Client(ClientConfig config, ClientEvents events)
    : config_(std::move(config)), events_(std::move(events)) {}

Client::~Client() { close(); }

double Client::now() const { return steady_seconds(); }

void Client::log(const std::string& text) const {
    if (events_.on_log) events_.on_log(text);
}

bool Client::connect(std::string& error) {
    close();
    tcp_ = net::connect_tcp(config_.host, config_.port);
    if (tcp_ == net::kBad) {
        error = "cannot reach " + config_.host + ":" + std::to_string(config_.port);
        return false;
    }
    session_ = random_hex(8);
    std::string hello = hello_line(config_.key, config_.name, session_, random_hex(8));
    if (net::send_some(tcp_, hello.data(), hello.size()) != long(hello.size())) {
        error = "could not send the hello message";
        close();
        return false;
    }

    net::set_nonblocking(tcp_);
    double deadline = now() + kHandshakeTimeoutS;
    std::string line;
    while (true) {
        char buffer[4096];
        long got = net::recv_some(tcp_, buffer, sizeof buffer);
        if (got > 0) {
            rx_.append(buffer, std::size_t(got));
            std::size_t end = rx_.find('\n');
            if (end != std::string::npos) {
                line = rx_.substr(0, end);
                rx_.erase(0, end + 1);
                break;
            }
        } else if (got == 0 || !net::would_block()) {
            error = "the gateway closed the connection";
            close();
            return false;
        }
        if (now() > deadline) {
            error = "no answer to hello";
            close();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto answer = json_parse(line);
    if (!answer || !answer->is_object()) {
        error = "unreadable answer to hello";
        close();
        return false;
    }
    if (answer->string_or("t", "") != "welcome") {
        error = "refused: " + answer->string_or("reason", answer->string_or("t", "?"));
        close();
        return false;
    }
    if (const Json* caps = answer->find("capabilities")) capabilities_ = *caps;
    if (const Json* snapshot = answer->find("s")) state_ = *snapshot;
    state_version_ = answer->number_or("v", 0);

    udp_ = net::connect_udp(config_.host, std::uint16_t(answer->number_or("udp_port", 0)));
    if (udp_ == net::kBad) {
        error = "cannot open the drive stream";
        close();
        return false;
    }
    net::set_nonblocking(udp_);
    connected_ = true;
    next_stream_ = now();
    log("connected to " + capabilities_.string_or("name", "?"));
    return true;
}

void Client::close() {
    net::close_socket(tcp_);
    net::close_socket(udp_);
    tcp_ = udp_ = net::kBad;
    connected_ = false;
    rx_.clear();
    tx_.clear();
    pending_.clear();
}

void Client::lost(const std::string& why) {
    if (!connected_) return;
    log("connection lost: " + why);
    for (const Pending& p : pending_)
        if (events_.on_ack) events_.on_ack(p.id, "lost", why);
    close();
}

void Client::drive(double vx, double wz, bool deadman, std::vector<std::string> held) {
    drive_vx_ = vx;
    drive_wz_ = wz;
    drive_deadman_ = deadman;
    drive_held_ = std::move(held);
}

std::uint32_t Client::action(const std::string& name, const std::string& op,
                             std::optional<bool> value, const Json& params, int ttl_ms) {
    Json message = Json::object();
    message["t"] = Json::of("action");
    message["id"] = Json::of(double(next_id_));
    message["name"] = Json::of(name);
    message["op"] = Json::of(op);
    message["ttl_ms"] = Json::of(ttl_ms);
    if (value) message["value"] = Json::of(*value);
    if (params.is_object() && !params.fields.empty()) message["params"] = params;

    Pending pending{next_id_++, message, now(), now(), ttl_ms};
    message["age_ms"] = Json::of(0);
    if (connected_) send_line(message);
    pending_.push_back(std::move(pending));
    return pending_.back().id;
}

std::uint32_t Client::start_long(const std::string& name, bool confirmed, int ttl_ms) {
    Json params = Json::object();
    params["confirmed"] = Json::of(confirmed);
    return action(name, "start", std::nullopt, params, ttl_ms);
}

void Client::cancel(std::uint32_t id) {
    Json message = Json::object();
    message["t"] = Json::of("cancel");
    message["id"] = Json::of(double(id));
    if (connected_) send_line(message);
}

void Client::send_line(const Json& message) {
    tx_ += json_dump(message) + "\n";
    flush_tx();
}

void Client::flush_tx() {
    while (!tx_.empty() && tcp_ != net::kBad) {
        long sent = net::send_some(tcp_, tx_.data(), tx_.size());
        if (sent > 0) {
            tx_.erase(0, std::size_t(sent));
        } else if (sent < 0 && net::would_block()) {
            return;
        } else {
            lost("send failed");
            return;
        }
    }
}

void Client::pump_rx() {
    char buffer[4096];
    while (connected_) {
        long got = net::recv_some(tcp_, buffer, sizeof buffer);
        if (got > 0) {
            rx_.append(buffer, std::size_t(got));
        } else if (got == 0) {
            lost("closed by the gateway");
            return;
        } else if (net::would_block()) {
            break;
        } else {
            lost("receive failed");
            return;
        }
    }
    std::size_t end;
    while ((end = rx_.find('\n')) != std::string::npos) {
        std::string line = rx_.substr(0, end);
        rx_.erase(0, end + 1);
        if (auto message = json_parse(line); message && message->is_object()) dispatch(*message);
    }
}

void Client::dispatch(const Json& message) {
    std::string kind = message.string_or("t", "");
    auto id = std::uint32_t(message.number_or("id", 0));
    std::string status = message.string_or("status", "");
    std::string reason = message.string_or("reason", "");

    if (kind == "state") {
        double version = message.number_or("v", -1);
        if (version > state_version_) {
            state_version_ = version;
            if (const Json* snapshot = message.find("s")) state_ = *snapshot;
        }
    } else if (kind == "ack") {
        auto it = std::find_if(pending_.begin(), pending_.end(),
                               [id](const Pending& p) { return p.id == id; });
        if (it != pending_.end()) {
            pending_.erase(it);
            if (events_.on_ack) events_.on_ack(id, status, reason);
        }
    } else if (kind == "progress") {
        if (events_.on_progress) events_.on_progress(id, message.number_or("fraction", 0));
    } else if (kind == "result") {
        if (events_.on_result) events_.on_result(id, status, reason);
    } else if (kind == "error") {
        log("gateway error: " + reason);
    }
}

void Client::resend_pending() {
    double t = now();
    for (std::size_t i = 0; i < pending_.size() && connected_;) {
        Pending& p = pending_[i];
        if ((t - p.created) * 1000 >= p.ttl_ms) {
            std::uint32_t id = p.id;
            pending_.erase(pending_.begin() + std::ptrdiff_t(i));
            if (events_.on_ack) events_.on_ack(id, "timeout", "no answer in time");
            continue;
        }
        if (t - p.last_sent >= config_.retry_s) {
            p.last_sent = t;
            p.message["age_ms"] = Json::of(int((t - p.created) * 1000));
            log("retry " + p.message.string_or("name", "?"));
            Json copy = p.message;
            send_line(copy);
        }
        ++i;
    }
}

void Client::send_stream() {
    double t = now();
    if (paused_ || t < next_stream_) return;
    next_stream_ += 1.0 / config_.rate_hz;
    if (next_stream_ < t - 1.0) next_stream_ = t;

    StreamPacket packet{session_, ++seq_, drive_vx_, drive_wz_, drive_deadman_, drive_held_};
    std::string data = encode_stream(config_.key, packet);
    net::send_some(udp_, data.data(), data.size());
}

void Client::update() {
    if (!connected_) return;
    pump_rx();
    if (!connected_) return;
    flush_tx();
    resend_pending();
    if (connected_) send_stream();
}

}  // namespace sw
