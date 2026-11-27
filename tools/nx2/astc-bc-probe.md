# Offline Vulkan ASTC-to-BC comparison

The probe compiles Lichii's production `astc_decoder.comp` and `bc_encoder.comp` from Git, then
runs them on the same block-linear ASTC data as a fused `astc_bc.comp`. The two-pass path uses a
real `VK_FORMAT_R8G8B8A8_UNORM` image between the dispatches, even for sRGB data. The fused path
uses the shader's production specialization constants and push constants. The probe validates
SPIR-V and enables Vulkan synchronization validation.

On Ubuntu 24.04:

```sh
sudo apt-get install g++ libvulkan-dev mesa-vulkan-drivers vulkan-validationlayers \
    glslang-tools spirv-tools libastcenc-dev
python3 -I tools/nx2/test_astc_bc.py --reference-ref 148dd9c3c4 --extended --out /tmp/astc-probe
```

The default candidate is the working tree's fused shader. Use `--candidate-dir DIRECTORY`
to test another shader directory, or `--candidate-ref REV` to compile from a Git revision.
The untouched baseline defaults to `origin/nx2`. Use
`--reference-dir DIRECTORY` to test a refactored fallback and compare the resulting
`previous.bc` files from both runs with `cmp`.

To reproduce the upstream comparison, fetch [Eglantine #951](https://github.com/Eglantine-Community/Eglantine/pull/951)
and pass `--candidate-ref 1f6f62c82faeac8f3e433f7d4a25425ac3f97445 --allow-mismatch`.

The tool selects lavapipe automatically. Set `VK_DRIVER_FILES` if its ICD JSON is elsewhere.
Exit status 1 means at least one BC byte differed; `--allow-mismatch` reports those differences
without failing. Shader compilation, Vulkan validation, and other errors still fail.

## Coverage and outputs

The main matrix covers the ASTC footprints declared by Lichii, ASTCenc LDR linear and LDR sRGB,
and both BC1 and BC3. Each case contains four layers with opaque gradients, noisy color, mixed
opacity, and constant ASTC blocks. One legacy 64×48 texture and one Switch 2 129×109 texture per
case exercise both GOB layouts and full mip chains down to 1×1. ASTCenc checks the generated
blocks, including the constant blocks whose 16-bit values the probe adjusts to test non-replicated
low bytes. `--quick` limits the matrix to 4×4, 8×5, and 12×12. `--extended` adds a 4×4 HDR CEM15
fixture with alpha 0.5 and 16-bit LDR void extents on both sides of UNORM8 rounding thresholds.
The extended cases have separate totals and don't contribute to the main matrix totals.

- `run.log` reports total blocks and bytes, different blocks and bytes, the first mismatch for
  each case, and the input ASTC and output BC mode distributions.
- `visible_changed` means at least one in-bounds texel differs under a rational expansion of
  the BC palettes. `padding_only` means only out-of-bounds texels of a partial block differ.
  `equivalent` means the bytes differ but expand to the same texels. `changed_edge` counts all
  different partial blocks, regardless of the classification.
- `previous.bc` and `candidate.bc` store every output block. They follow the case order in
  `run.log`, then mip, layer, row, and column order within each case. `provenance.json` records
  the two revisions or directories, the shader source hashes, and the output SHA-256 values.

The two summed Vulkan timestamp intervals include dispatches and the RGBA8 transition/barrier
where applicable, after one warmup per BC format. They're indicative lavapipe timings, not
emulator frame times or discrete-GPU measurements. The probe does not exercise emulator
allocation, GPU scheduling, the final buffer-to-image copy, or hardware BC sampling precision.

## Why the port keeps Lichii's codec

Upstream #951 changed 11,966 of 332,864 LDR output blocks against the Lichii reference on Mesa
25.2.8 lavapipe. None of those differences were confined to padding. Its decoder treats sRGB void
extents differently and does not preserve Lichii's HDR color endpoints or invalid-block fill.
The port shares Lichii's ASTC decoder and BC encoder with the two-pass path instead.

Fused encoding packs each decoded texel to RGBA8, the same byte format as the old intermediate
image. It then multiplies each byte by the FP32 reciprocal of 255, as lavapipe's storage-image
read does. The `precise` qualifier prevents the compiler from folding that multiplication into
the BC3 alpha fit's multiplication by 255. Without that boundary, alpha palette indices differed
even for matching RGBA8 texels. The final probe matched all 333,120 output blocks, including the
HDR and UNORM threshold cases. Vulkan permits implementation-dependent UNORM conversion rounding,
so this is not a claim about untested drivers. For example, a decoded alpha of 0.5 lies between
127 and 128 and can change both BC3 alpha bytes and BC1 punch-through.

The AIO-17 test build uses the two-pass path by default. Set `EDEN_NX2_ASTC_FUSED=1` before launch
to enable fusion for textures already eligible for Vulkan GPU recompression. Set
`EDEN_NX2_TRANSCODE_LOG=1` in both arms to log fused and two-pass counts, BC output, and estimated
RGBA8 payload and peak decode-image usage.
