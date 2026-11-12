#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace imza {

// SHA-256 (FIPS 180-4), local so no crypto dependency enters the link.
// Consumers today: PKCE challenges (mcp_oauth) and SigV4 signing (aws_sign).
inline std::array<std::uint8_t, 32> sha256(std::string_view input)
{
    constexpr std::uint32_t K[] = {
        0x428a2f98,
        0x71374491,
        0xb5c0fbcf,
        0xe9b5dba5,
        0x3956c25b,
        0x59f111f1,
        0x923f82a4,
        0xab1c5ed5,
        0xd807aa98,
        0x12835b01,
        0x243185be,
        0x550c7dc3,
        0x72be5d74,
        0x80deb1fe,
        0x9bdc06a7,
        0xc19bf174,
        0xe49b69c1,
        0xefbe4786,
        0x0fc19dc6,
        0x240ca1cc,
        0x2de92c6f,
        0x4a7484aa,
        0x5cb0a9dc,
        0x76f988da,
        0x983e5152,
        0xa831c66d,
        0xb00327c8,
        0xbf597fc7,
        0xc6e00bf3,
        0xd5a79147,
        0x06ca6351,
        0x14292967,
        0x27b70a85,
        0x2e1b2138,
        0x4d2c6dfc,
        0x53380d13,
        0x650a7354,
        0x766a0abb,
        0x81c2c92e,
        0x92722c85,
        0xa2bfe8a1,
        0xa81a664b,
        0xc24b8b70,
        0xc76c51a3,
        0xd192e819,
        0xd6990624,
        0xf40e3585,
        0x106aa070,
        0x19a4c116,
        0x1e376c08,
        0x2748774c,
        0x34b0bcb5,
        0x391c0cb3,
        0x4ed8aa4a,
        0x5b9cca4f,
        0x682e6ff3,
        0x748f82ee,
        0x78a5636f,
        0x84c87814,
        0x8cc70208,
        0x90befffa,
        0xa4506ceb,
        0xbef9a3f7,
        0xc67178f2,
    };
    std::uint32_t state[] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    const auto rotate = [](std::uint32_t value, unsigned count) {
        return (value >> count) | (value << (32 - count));
    };

    std::string message(input);
    const std::uint64_t bit_length = message.size() * 8ULL;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) {
        message.push_back('\0');
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bit_length >> shift) & 0xff));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t w[64];
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint8_t>(message[offset + i * 4]) << 24)
                | (static_cast<std::uint8_t>(message[offset + i * 4 + 1]) << 16)
                | (static_cast<std::uint8_t>(message[offset + i * 4 + 2]) << 8)
                | static_cast<std::uint8_t>(message[offset + i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate(w[i - 15], 7)
                ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19)
                ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3],
                      e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t sum1
                = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1  = h + sum1 + choice + K[i] + w[i];
            const std::uint32_t sum0
                = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2    = sum0 + majority;
            h                            = g;
            g                            = f;
            f                            = e;
            e                            = d + temp1;
            d                            = c;
            c                            = b;
            b                            = a;
            a                            = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint8_t, 32> digest { };
    for (std::size_t i = 0; i < 8; ++i) {
        for (int byte = 0; byte < 4; ++byte) {
            digest[i * 4 + byte]
                = static_cast<std::uint8_t>(state[i] >> (24 - byte * 8));
        }
    }
    return digest;
}

} // namespace imza