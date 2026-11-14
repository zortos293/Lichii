#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile and embed the inference kernels. No runtime toolchain or asset directory."""
import argparse
import pathlib
import struct
import subprocess
import sys


def main():
    """Compile shaders and generate PTX, then atomically replace the embedded C++ asset table."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--glslang', required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    intermediates = output.parent / 'kernels'
    intermediates.mkdir(exist_ok=True)
    assets = []
    for shader in sorted((root / 'shaders').glob('*.comp')):
        result = intermediates / (shader.stem + '.spv')
        subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.3',
                        '-I' + str(root / 'shaders'), str(shader), '-o', str(result)], check=True)
        assets.append(result)

    def emit(script, name, before=(), after=()):
        result = intermediates / (name + '.ptx')
        subprocess.run([sys.executable, '-B', str(root / 'scripts/ptx' / (script + '.py')),
                        *map(str, before), str(result), *map(str, after)],
                       stdout=subprocess.DEVNULL, check=True)
        assets.append(result)

    for k in (64, 128, 256):
        emit('mlp_e4m3', f'mlp_e4m3_K{k}', (k,), (3, 1, 1))
    for c in (64, 128, 256, 512):
        emit('qkv_e4m3', f'qkv_e4m3_K{c}', (c,), (80,))
    for k in (32, 64, 128, 256, 512, 1024):
        for f in (5, 13, 4, 8, 6):
            emit('gemm2_e4m3', f'gemm2_e4m3_K{k}_f{f}', (k, f))
    for c, r, shared in ((64, 4, 0), (128, 4, 0), (256, 3, 80), (256, 4, 64)):
        emit('ffn_e4m3', f'ffn_e4m3_C{c}_R{r}', (c, r), (shared,))
        emit('ffn_e4m3', f'ffn_e4m3_C{c}_R{r}_proj', (c, r), (shared, 1))
    for k, f, p, s in ((4096, 1, 32, 4), (1024, 0, 16, 2), (1024, 1, 8, 4), (1024, 0, 8, 4)):
        emit('gemmt_e4m3', f'gemmt_e4m3_K{k}_f{f}_p{p}_s{s}', (k, f, p, s))
    for s, f in ((4, 4), (2, 8), (4, 8)):
        emit('reduce_e4m3', f'reduce_e4m3_s{s}_f{f}', (s, f))
    for k, f, s in ((1024, 6, 1), (4096, 5, 4), (1024, 8, 2), (1024, 5, 4)):
        emit('gemmv_e4m3', f'gemmv_e4m3_K{k}_f{f}_s{s}', (k, f, s))
        emit('gemmv_e4m3', f'gemmv_e4m3_K{k}_f{f}_s{s}_m96', (k, f, s), (4, 6, 0, 0, 0, 96))
    for p in (64, 128, 192, 256):
        emit('global_attention_e4m3', f'global_attention_e4m3_p{p}', (p,))
    emit('global_attention_stream_e4m3', 'global_normalize_e4m3', ('normalize',))
    emit('global_attention_stream_e4m3', 'global_attention_stream_e4m3', ('attention',), (4,))
    for k, f in ((512, 4), (512, 5), (512, 13), (256, 5), (4096, 5), (1024, 8), (1024, 5)):
        emit('gemmv_e4m3', f'gemmv_e4m3_K{k}_f{f}_s1_m96', (k, f, 1), (4, 6, 0, 0, 0, 96))
    for f in (2, 66, 74, 130, 48):
        emit('block32_e4m3', f'block32_e4m3_f{f}', (f,))

    # Preserve SPIR-V alignment and embed PTX as text, including the terminating zero.
    generated = output.with_suffix('.tmp')
    with generated.open('w') as out:
        out.write('#include "assets.h"\n#include <cstdint>\n#include <stdexcept>\n#include <string>\nnamespace nr {\nnamespace {\n')
        for i, asset in enumerate(assets):
            if asset.suffix == '.spv':
                words = struct.unpack('<' + 'I' * (asset.stat().st_size // 4), asset.read_bytes())
                out.write(f'const uint32_t asset{i}[] = {{\n')
                for start in range(0, len(words), 12):
                    out.write(','.join(hex(word) for word in words[start:start + 12]) + ',\n')
                out.write('};\n')
            else:
                contents = asset.read_text()
                if ')NR_PTX"' in contents:
                    raise ValueError(f'{asset.name} contains the raw-string delimiter')
                out.write(f'const char asset{i}[] = R"NR_PTX({contents})NR_PTX";\n')
        out.write('struct Asset { std::string_view name; const void* data; size_t size; };\nconst Asset assets[] = {\n')
        for i, asset in enumerate(assets):
            out.write(f'{{"{asset.name}", asset{i}, {asset.stat().st_size}}},\n')
        out.write('''};
}
std::span<const std::byte> FindAsset(std::string_view path) {
    path.remove_prefix(path.find_last_of("/\\\\") + 1);
    for (const auto& asset : assets)
        if (asset.name == path)
            return {static_cast<const std::byte*>(asset.data), asset.size};
    return {};
}
std::span<const std::byte> RequireAsset(std::string_view path) {
    const auto asset = FindAsset(path);
    if (asset.empty()) throw std::runtime_error("Missing embedded NR kernel: " + std::string{path});
    return asset;
}
}
''')
    generated.replace(output)


if __name__ == '__main__':
    main()
