// Wire format, mirroring src/switchy_op/protocol.py. See docs/protocol.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sw {

inline constexpr int kProto = 1;

struct StreamPacket {
    std::string session;
    std::uint32_t seq = 0;
    double vx = 0;  // -1..1
    double wz = 0;  // -1..1
    bool deadman = false;
    std::vector<std::string> held;
};

// Random lowercase hex (2 characters per byte). Not cryptographic: ids only need to be unique.
std::string random_hex(std::size_t bytes);

// One newline-terminated hello line, signed with the shared key.
std::string hello_line(const std::string& key, const std::string& client_name,
                       const std::string& session, const std::string& nonce);

// One UDP datagram body for the drive stream, signed with the shared key.
std::string encode_stream(const std::string& key, const StreamPacket& packet);

}  // namespace sw
