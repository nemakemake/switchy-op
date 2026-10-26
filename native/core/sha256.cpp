#include "sha256.hpp"

#include <cstring>
#include <vector>

namespace sw {
namespace {

constexpr std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void compress(std::uint32_t state[8], const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t choose = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + s1 + choose + kRound[i] + w[i];
        std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

}  // namespace

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t length) {
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t full = length / 64;
    for (std::size_t i = 0; i < full; ++i) compress(state, data + i * 64);

    std::uint8_t tail[128] = {};
    std::size_t rest = length - full * 64;
    if (rest != 0) std::memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    std::size_t tail_len = rest < 56 ? 64 : 128;
    std::uint64_t bits = std::uint64_t(length) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = std::uint8_t(bits >> (8 * i));
    for (std::size_t offset = 0; offset < tail_len; offset += 64) compress(state, tail + offset);

    std::array<std::uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = std::uint8_t(state[i] >> 24);
        digest[i * 4 + 1] = std::uint8_t(state[i] >> 16);
        digest[i * 4 + 2] = std::uint8_t(state[i] >> 8);
        digest[i * 4 + 3] = std::uint8_t(state[i]);
    }
    return digest;
}

std::string hmac_sha256_hex(std::string_view key, std::string_view message, std::size_t chars) {
    std::uint8_t block[64] = {};
    if (key.size() > 64) {
        auto hashed = sha256(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
        std::memcpy(block, hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(block, key.data(), key.size());
    }

    std::vector<std::uint8_t> inner(64 + message.size());
    std::vector<std::uint8_t> outer(64 + 32);
    for (int i = 0; i < 64; ++i) {
        inner[i] = block[i] ^ 0x36;
        outer[i] = block[i] ^ 0x5c;
    }
    if (!message.empty()) std::memcpy(inner.data() + 64, message.data(), message.size());
    auto inner_hash = sha256(inner.data(), inner.size());
    std::memcpy(outer.data() + 64, inner_hash.data(), inner_hash.size());
    auto digest = sha256(outer.data(), outer.size());

    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (std::uint8_t byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 15]);
    }
    hex.resize(chars < hex.size() ? chars : hex.size());
    return hex;
}

}  // namespace sw
