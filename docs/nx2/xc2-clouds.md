# Xenoblade Chronicles 2 cloud rendering

This workaround is restricted to Vulkan and title `0100E95004038000`. Other
title IDs remain outside the policy until separately validated.

On AMD, at native resolution and single-sample triangle rasterization, matching
smooth position varyings can be reconstructed from fragment coordinates and
the actual host viewport. The pass requires unique matching vertex stores of
x/y/w, either identical to position or related by a provably draw-uniform
offset. It carries offsets in an unused flat varying. Indexed attributes,
viewport selection, render-area coordinates, tessellation and geometry paths
are excluded. Unknown offset sources and Phi nodes fail closed. The approach
is adapted from [Eden PR 4575](https://git.eden-emu.dev/eden-emu/eden/pulls/4575);
this port does not include its barycentric or query-cache changes.

When native D24 attachment support is absent, eligible fragment texture reads
are normalized to D24 precision. Runtime image-view depth aspect and guest
format determine eligibility independently of the shader's initial binding.
Non-comparison samples additionally require the effective nearest min/mag/mip
sampler with no anisotropy, border addressing or reduction mode. Integer,
stencil, color and guest D32 views remain unquantized; unsupported operations,
dynamic descriptor arrays and mask indices beyond 127 are excluded. Samples
stay outside the helper's conditional branch to retain derivative behavior.
The helper bypasses quantization arithmetic when its runtime mask is clear.
Runtime view/sampler classification is instantiated only for stages that use
these reads; D24 masks are initialized lazily and pushed only on affected
pipelines. These restrictions do not establish zero CPU or GPU overhead.

The packed-to-float normalization follows the existing X8D24 conversion rather
than multiplying by a rounded reciprocal of 16777215. Normalized values are
rounded to the 24-bit integer grid; NaN payloads and out-of-range inputs are
returned unchanged. No render target is rewritten or cloud effect disabled.

`tests '[xbc2]'` exercises synthetic IR, conservative guards, viewport offsets,
depth/stencil swizzles, descriptor rebinding, sampler restrictions, mask bounds,
normalization boundaries and helper calls inside caller loops/selections.
`XBC2_TEST_SPIRV_DIR=<temporary directory>` exports only generated synthetic
modules for external `spirv-val --target-env vulkan1.1` validation. Optional
compute fixtures produce 512-byte SSBO outputs for 64 invocations; use
`tools/nx2/verify_xbc2_d24_fixture.py` to compare their outputs bit exactly.
No commercial game shader, texture, capture or executable is included.

The disk shader cache stores guest environments and pipeline keys; it translates
and emits shaders again when loading them. This change does not alter that
serialization format. Vulkan driver caches key the newly emitted shader and
pipeline state. Cache versions and compatible-cache migrations are unchanged;
no UI toggle or experiment environment switch is part of the proposed fix.

The user reports a substantial improvement in the broader local combined build
on a ROG Ally Z1 Extreme (16 GB), SteamOS 3.9.5, RADV Mesa 26.2.6, after over an
hour of play through chapter 1. This is not a definitive fix claim or independent
validation of each component. The extracted implementation passed four CTest
suites and validation of 39 synthetic SPIR-V modules on base 9fa4a71f, with
separate local diagnostic overlays. Those overlays are excluded from this patch.
A single pair of 60-second manual cloud-area walks on that base measured
29.30 FPS without and 29.08 FPS with both fixes (-0.74%). The small difference
cannot be attributed to the fixes from this test; capped FPS does not prove zero
GPU cost. The production patch is proposed on a newer base; its CI remains
separate from that earlier validation.
