// SHA-256 and HMAC-SHA256, self-contained so the same code builds on a PC and on the Switch.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sw {

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t length);

// Lower-case hex of HMAC-SHA256(key, message), cut to `chars` characters (max 64).
std::string hmac_sha256_hex(std::string_view key, std::string_view message, std::size_t chars = 64);

}  // namespace sw
