// Gateway client: handshake, drive stream and reliable actions. Single-threaded: call update()
// once per frame from the main loop.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"
#include "net.hpp"
#include "protocol.hpp"

namespace sw {

struct ClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;
    std::string key = "dev-key";
    std::string name = "switchy-nx";
    double rate_hz = 30.0;   // drive stream rate
    double retry_s = 0.25;   // resend an unanswered action after this long
};

struct ClientEvents {
    std::function<void(std::uint32_t id, const std::string& status, const std::string& reason)> on_ack;
    std::function<void(std::uint32_t id, double fraction)> on_progress;
    std::function<void(std::uint32_t id, const std::string& status, const std::string& reason)> on_result;
    std::function<void(const std::string& text)> on_log;
};

class Client {
public:
    explicit Client(ClientConfig config, ClientEvents events = {});
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Blocks for up to a few seconds. On failure `error` says why (refused, auth, network).
    bool connect(std::string& error);
    void close();
    bool connected() const { return connected_; }

    // What the stream sends from now on: vx and wz in -1..1.
    void drive(double vx, double wz, bool deadman, std::vector<std::string> held);
    void set_stream_paused(bool paused) { paused_ = paused; }

    // Reliable actions; the answer arrives through the events. Returns the action id.
    std::uint32_t action(const std::string& name, const std::string& op,
                         std::optional<bool> value = std::nullopt, const Json& params = Json(),
                         int ttl_ms = 1500);
    std::uint32_t start_long(const std::string& name, bool confirmed, int ttl_ms = 1500);
    void cancel(std::uint32_t id);

    // Call every frame: reads the network, resends, and sends the drive stream when due.
    void update();

    const Json& state() const { return state_; }
    const Json& capabilities() const { return capabilities_; }

private:
    struct Pending {
        std::uint32_t id;
        Json message;
        double created;
        double last_sent;
        int ttl_ms;
    };

    double now() const;
    void log(const std::string& text) const;
    void send_line(const Json& message);
    void flush_tx();
    void pump_rx();
    void dispatch(const Json& message);
    void resend_pending();
    void send_stream();
    void lost(const std::string& why);

    ClientConfig config_;
    ClientEvents events_;
    net::Socket tcp_ = net::kBad;
    net::Socket udp_ = net::kBad;
    bool connected_ = false;
    bool paused_ = false;
    std::string session_;
    std::string rx_;
    std::string tx_;
    std::vector<Pending> pending_;
    std::uint32_t next_id_ = 1;
    std::uint32_t seq_ = 0;
    double next_stream_ = 0;
    double drive_vx_ = 0, drive_wz_ = 0;
    bool drive_deadman_ = false;
    std::vector<std::string> drive_held_;
    Json state_ = Json::object();
    Json capabilities_ = Json::object();
    double state_version_ = -1;
};

}  // namespace sw
