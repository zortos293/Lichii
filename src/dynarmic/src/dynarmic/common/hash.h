// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <span>

namespace Dynarmic::Common {

/// Non-cryptographic 128-bit streaming hash (two independent 64-bit lanes, each a multiply and
/// xor-shift mix per 8-byte word, ~5 GB/s): for identifying files and checking data for damage.
/// The result depends only on the bytes, not on how they were split across Update calls.
class Hash128 {
public:
    void Update(std::span<const std::uint8_t> bytes) {
        if (pending_size != 0) {
            const size_t take = std::min(bytes.size(), pending.size() - pending_size);
            std::memcpy(pending.data() + pending_size, bytes.data(), take);
            pending_size += take;
            bytes = bytes.subspan(take);
            if (pending_size < pending.size()) {
                return;
            }
            Word(Load(pending.data()));
            pending_size = 0;
        }
        const size_t words = bytes.size() / 8;
        for (size_t i = 0; i < words; ++i) {
            Word(Load(bytes.data() + i * 8));
        }
        pending_size = bytes.size() - words * 8;
        std::memcpy(pending.data(), bytes.data() + words * 8, pending_size);
    }

    std::array<std::uint64_t, 2> Finish() const {
        std::uint64_t a = lane_a;
        std::uint64_t b = lane_b;
        if (pending_size != 0) {
            std::array<std::uint8_t, 8> tail{};
            std::memcpy(tail.data(), pending.data(), pending_size);
            const std::uint64_t word = Load(tail.data());
            a = Mix(a ^ word);
            b = Mix(b + word * k1);
        }
        const std::uint64_t length = total + pending_size;
        a = Mix(a ^ length);
        b = Mix(b ^ (length * k0));
        return {Mix(a ^ (b >> 29)), Mix(b ^ (a >> 31))};
    }

    static std::array<std::uint64_t, 2> Of(std::span<const std::uint8_t> bytes) {
        Hash128 hash;
        hash.Update(bytes);
        return hash.Finish();
    }

private:
    static constexpr std::uint64_t k0 = 0x9E37'79B9'7F4A'7C15ULL;
    static constexpr std::uint64_t k1 = 0xC2B2'AE3D'27D4'EB4FULL;

    static std::uint64_t Load(const std::uint8_t* p) {
        std::uint64_t value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }
    /// splitmix64's finaliser.
    static std::uint64_t Mix(std::uint64_t x) {
        x ^= x >> 30;
        x *= 0xBF58'476D'1CE4'E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D0'49BB'1331'11EBULL;
        x ^= x >> 31;
        return x;
    }
    void Word(std::uint64_t word) {
        lane_a = Mix(lane_a ^ word);
        lane_b = Mix(lane_b + word * k1);
        total += 8;
    }

    std::uint64_t lane_a = k0;
    std::uint64_t lane_b = k1;
    std::uint64_t total = 0;
    std::array<std::uint8_t, 8> pending{};
    size_t pending_size = 0;
};

}  // namespace Dynarmic::Common
