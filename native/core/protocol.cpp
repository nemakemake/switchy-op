#include "protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "json.hpp"
#include "sha256.hpp"

namespace sw {
namespace {

// -0.0000 and 0.0000 would print differently, so fold tiny values to a clean zero.
std::string fixed4(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.4f", value);
    std::string text = buffer;
    if (text == "-0.0000") text = "0.0000";
    return text;
}

std::string stream_text(const StreamPacket& p, const std::string& vx, const std::string& wz) {
    std::vector<std::string> held = p.held;
    std::sort(held.begin(), held.end());
    std::string joined;
    for (std::size_t i = 0; i < held.size(); ++i) joined += (i ? "," : "") + held[i];
    return p.session + ":" + std::to_string(p.seq) + ":" + vx + ":" + wz + ":" +
           (p.deadman ? "1" : "0") + ":" + joined;
}

}  // namespace

std::string random_hex(std::size_t bytes) {
    static std::uint64_t counter = 0;
    int local = 0;
    std::uint64_t seed =
        std::uint64_t(std::chrono::system_clock::now().time_since_epoch().count()) ^
        (std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) << 1) ^
        (std::uint64_t(reinterpret_cast<std::uintptr_t>(&local)) << 7) ^ (++counter * 0x9e3779b97f4a7c15ULL);
    std::string out;
    for (std::uint64_t block = 0; out.size() < bytes * 2; ++block) {
        std::uint8_t input[16];
        for (int i = 0; i < 8; ++i) {
            input[i] = std::uint8_t(seed >> (8 * i));
            input[8 + i] = std::uint8_t(block >> (8 * i));
        }
        auto digest = sha256(input, sizeof input);
        static const char kHex[] = "0123456789abcdef";
        for (std::uint8_t byte : digest) {
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 15]);
        }
    }
    out.resize(bytes * 2);
    return out;
}

std::string hello_line(const std::string& key, const std::string& client_name,
                       const std::string& session, const std::string& nonce) {
    Json hello = Json::object();
    hello["t"] = Json::of("hello");
    hello["proto"] = Json::of(kProto);
    hello["client"] = Json::of(client_name);
    hello["session"] = Json::of(session);
    hello["nonce"] = Json::of(nonce);
    hello["mac"] = Json::of(hmac_sha256_hex(key, "hello:" + session + ":" + nonce, 32));
    return json_dump(hello) + "\n";
}

std::string encode_stream(const std::string& key, const StreamPacket& packet) {
    std::string vx = fixed4(std::max(-1.0, std::min(1.0, packet.vx)));
    std::string wz = fixed4(std::max(-1.0, std::min(1.0, packet.wz)));
    std::string mac = hmac_sha256_hex(key, stream_text(packet, vx, wz), 16);

    std::vector<std::string> held = packet.held;
    std::sort(held.begin(), held.end());
    std::string out = "{\"s\":" + json_dump(Json::of(packet.session)) +
                      ",\"q\":" + std::to_string(packet.seq) + ",\"vx\":" + vx + ",\"wz\":" + wz +
                      ",\"dm\":" + (packet.deadman ? "true" : "false") + ",\"h\":[";
    for (std::size_t i = 0; i < held.size(); ++i)
        out += (i ? "," : "") + json_dump(Json::of(held[i]));
    out += "],\"mac\":\"" + mac + "\"}";
    return out;
}

}  // namespace sw
