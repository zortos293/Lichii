// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace TensorBench {

enum class Mode { Software, Cooperative };
enum class Pattern { Integers, Fractions };

struct Parameters {
    Mode mode{Mode::Software};
    Pattern pattern{Pattern::Integers};
    std::uint32_t depth{16};
    std::uint32_t operations{1};
    std::uint32_t local_size{64};
};

struct ShaderModule {
    std::vector<std::uint32_t> words;
    std::uint32_t shared_memory_bytes{};
};

struct Fixture {
    std::vector<std::uint32_t> input;
    std::vector<std::uint32_t> expected;
};

// One storage buffer at set 0, binding 0: eight input and two output words per invocation.
inline constexpr std::uint32_t WordsPerInvocation = 10;

ShaderModule EmitShader(const Parameters& parameters);
Fixture MakeFixture(const Parameters& parameters, std::uint32_t workgroups, std::uint32_t seed);
bool EquivalentWord(std::uint32_t actual, std::uint32_t expected);

} // namespace TensorBench
