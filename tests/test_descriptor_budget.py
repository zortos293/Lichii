#!/usr/bin/env python3
"""Game-free regression for sampled-image budgets with texel-buffer arrays.

Extracts the production budget helper and compiles it with minimal descriptor
types. Run: python3 tests/test_descriptor_budget.py
No game assets or Vulkan runtime are used.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/shader_recompiler/ir_opt/texture_pass.cpp").read_text()
info = (root / "src/shader_recompiler/shader_info.h").read_text()

def extract(code, signature):
    start = code.index(signature)
    brace = code.index("{", start)
    depth = 0
    for pos in range(brace, len(code)):
        if code[pos] == "{":
            depth += 1
        elif code[pos] == "}":
            depth -= 1
            if depth == 0:
                return code[start:pos + 1]
    raise AssertionError("Function is not closed: " + signature)

cpp = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <vector>
using u32 = uint32_t;
struct TextureBufferDescriptor { u32 count; };
struct TextureDescriptor { u32 count; bool is_descriptor_array; };
struct Info {
    std::vector<TextureBufferDescriptor> texture_buffer_descriptors;
    std::vector<TextureDescriptor> texture_descriptors;
    std::vector<TextureDescriptor> constant_buffer_descriptors;
    std::vector<TextureDescriptor> storage_buffers_descriptors;
    std::vector<TextureDescriptor> image_buffer_descriptors;
    std::vector<TextureDescriptor> image_descriptors;
};
struct HostTranslateInfo {
    u32 max_per_stage_descriptor_sampled_images = 16;
    u32 max_descriptor_set_sampled_images = 16;
    u32 max_per_stage_descriptor_samplers = 16;
    u32 max_descriptor_set_samplers = 16;
    u32 max_per_stage_resources = 64;
};
constexpr u32 DESCRIPTOR_MAX_COUNT = 1024;
namespace {
"""
for signature in (
    "[[nodiscard]] bool IsDescriptorArray(const TextureBufferDescriptor&",
    "[[nodiscard]] bool IsDescriptorArray(const TextureDescriptor&",
):
    if signature in source:
        cpp += extract(source, signature) + "\n"
cpp += "template <typename T>\n" + extract(source, "[[nodiscard]] u32 StaticDescriptorCount(")
cpp += "\ntemplate <typename Descriptors>\n" + extract(info, "u32 NumDescriptors(")
cpp += "\n" + extract(source, "u32 SaturatingSub(")
cpp += "\n" + extract(source, "u32 DynamicSampledTextureCap(")
cpp += r"""
} // anonymous namespace
int main() {
    HostTranslateInfo host;
    Info i;
    i.texture_buffer_descriptors = {{4}};
    i.texture_descriptors = {{64,true}};
    auto cap = DynamicSampledTextureCap(i,host,1);
    assert(cap == 12);
    assert(cap + NumDescriptors(i.texture_buffer_descriptors) <= 16);

    i.texture_buffer_descriptors = {{8}};
    i.texture_descriptors = {{1,false},{1,false},{64,true}};
    cap = DynamicSampledTextureCap(i,host,1);
    assert(cap == 6);
    assert(cap + NumDescriptors(i.texture_buffer_descriptors) + 2 <= 16);

    i.texture_descriptors = {{64,true},{64,true}};
    cap = DynamicSampledTextureCap(i,host,2);
    assert(cap == 4);
    assert(2*cap + NumDescriptors(i.texture_buffer_descriptors) == 16);

    host.max_per_stage_descriptor_samplers = 5;
    host.max_descriptor_set_samplers = 5;
    assert(DynamicSampledTextureCap(i,host,2) == 2);

    host.max_per_stage_descriptor_samplers = 16;
    host.max_descriptor_set_samplers = 16;
    host.max_per_stage_resources = 12;
    assert(DynamicSampledTextureCap(i,host,2) == 2);
}
"""
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp) / "test.cpp"
    path.write_text(cpp)
    compilers = [shutil.which(name) for name in ("g++", "clang++")]
    compilers = [exe for exe in compilers if exe]
    if not compilers:
        print("SKIP: no GCC or Clang C++ driver on this platform")
    for exe in compilers:
        output = Path(temp) / Path(exe).name
        flags = ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror=unused-function"]
        # Sanitizers available in the development environments used for the check.
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run([exe, *flags, str(path), "-o", str(output)], check=True)
        subprocess.run([str(output)], check=True)
        print(Path(exe).name, ": five descriptor budget checks passed")
