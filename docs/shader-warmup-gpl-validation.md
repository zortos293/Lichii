# Graphics shader warm-up + GPL validation (RDNA2 / RADV)

Date: 2026-11-14

This branch is a **validation roll-up** of the graphics warm-up work:

- Open PR #1177: `VK_EXT_graphics_pipeline_library` shader libraries.
- Closed PR #1183: graphics warm-up from the game files (`warm-graphics.bin`).
- Related: #579, #741, #1166, #1178, #601, #899.

It is based on build 177 (`87d84d5`). Current `nx2` has moved past that
(`#1619` removed the graphics precache list, `#1620` replaced the precache
marker with a content hash + variant), so this branch **is not ready to merge
as-is**. It is published with the test results below so the work can be ported
to current `nx2` without repeating the measurements.

The code changes in this branch are from PantACRO4life's #1177/#1183 branches
and their contributors. The validation measurements and this document are by
TuxArchi.

## Test system

- GPU: AMD Radeon RX 6650 XT (Navi 23, RDNA2, 8 GB)
- CPU / RAM: Ryzen 7 5800X / 32 GB
- OS: Arch Linux, kernel 7.2.14
- Driver: RADV / Mesa 26.2.6, Vulkan 1.4.354
- Emulator: Lichii build 177 (`87d84d5`) + this branch

## Configuration

```ini
precache_shaders=2                 # Always
build_all_cached_shaders=true
native_async_draws=1               # Bounded
use_graphics_pipeline_library=true
shader_warm_threads=0              # every CPU thread
```

Required on this GPU (existing `nx2` workaround, not changed by this branch):

```bash
EDEN_NX2_FORCE_MESH_FALLBACK=1
```

The log confirms:

```text
EDEN_NX2_FORCE_MESH_FALLBACK: VK_EXT_mesh_shader disabled
Nx2Perf gpl: graphics pipeline libraries on (fast linking)
```

## Results

### Builder

`Build Shader Precache` / `--build-warm` writes `warm-compute.bin` and
`warm-graphics.bin` from the user's own copy of the game in about 4 seconds.

```text
4794 compute, 22599 vertex, 37430 fragment entries, 51 programs skipped,
0 archive and 17456 member errors, 4.2 s
```

- `warm-graphics.bin`: 60,029 entries, 1,524,288,708 bytes (1.5 GB)
- `warm-compute.bin`: 4,794 entries, ~69 MB

### First warm-up (cold Lichii caches)

```text
Shader precache: 0 cached graphics pipelines and 4649 of 4794 compute programs
Shader precache: finished, 4649 built, 0 failed, 0 not reached, 145.6 s
Shader precache: graphics programs ... on 16 threads
Shader precache: graphics programs finished, 60029 built, 0 failed, ... 444.7 s
```

- Compute: 145.6 s
- Graphics: 444.7 s (7.4 min)
- Total: ~9.8 min
- 0 failed, 0 skipped
- `warm-compute.done` and `warm-graphics.done` written

A later roll-up run with Mesa's shader cache warm took 38.9 s for the same
60,029 entries. The long 444.7 s figure is the cold-driver case.

### Effect on the libraries built at boot

The same 2,021 libraries used by the boot/title path:

| | Before warm-up | After warm-up |
|---|---:|---:|
| pre-rasterization | 884 (80,807 ms, max 847.4 ms) | 884 (147 ms, max 7.6 ms) |
| fragment | 1,137 (139,595 ms, max 3,135.2 ms) | 1,137 (176 ms, max 7.1 ms) |
| total | ~220.4 s | ~0.32 s |

That is about **680x less driver time** for the same libraries, with the
worst case going from 3.1 s to 7.6 ms.

### Before/after context for new areas

On build 177 with the compute-only precache, a normal OoT session still built
574 pipelines in 8 minutes (p90 632 ms, max 3.5 s) and had 20 frames over
100 ms. The graphics warm-up is meant to cover those first visits. In-game
measurement on this branch is still pending; the boot/title measurements above
are the controlled part.

## Mesh fallback (`EDEN_NX2_FORCE_MESH_FALLBACK=1`)

`EDEN_NX2_FORCE_MESH_FALLBACK=1` is already in `nx2`; this branch does not
change it. It is required on this RDNA2 card because native
`VK_EXT_mesh_shader` reliably triggers the GPUVM/TDR fault tracked in #871 and
#1699.

The workaround is reported to help across several AMD series, not only Navi 23:

| Series | Cards with reports | Result |
|---|---|---|
| RDNA2 (Navi 21/23) | RX 6600, RX 6650 XT, RX 6800 XT, RX 6950 XT | Native mesh resets; compute fallback is stable and keeps foliage/geometry |
| RDNA3 (Navi 31/33/3.5) | RX 780M, RX 7900 XT, Radeon 8060S | Fallback is stable; native may work on Mesa >= 26.2.5 |
| RDNA4 (Navi 48) | RX 9070 XT, Radeon AI PRO R9700 | Fallback is stable; native may work on newer Mesa |

Sources: #871, #316, #1203, #1275, #1601, #1699, and the PR #814 matrix. PR
#814 is the path to make this choice automatic through a Mesh Shaders
setting; this validation branch only relies on the existing environment
variable.

With the fallback active, the full warm-up completed with no `ReportLoss`,
no `VK_ERROR_DEVICE_LOST`, and no `amdgpu` page fault/ring timeout.

## Caveats

- The warm-up covers the static vertex/fragment/compute programs in the game
  files. With the mesh compute fallback, the synthetic vertex stage generated
  by mesh emulation is not in the warm file; fragment libraries are still
  reused.
- The first cold warm-up is long (~10 min) and writes ~1.5 GB for
  `warm-graphics.bin` plus ~575 MB in the driver pipeline cache.
- This branch is based on build 177 and conflicts with current `nx2` in the
  marker/variant code (#1619/#1620). Porting it means folding the graphics
  marker version/mode into the current variant and bumping the builder version
  so #1577's stale-precache check sees `warm-graphics.bin`.
- Tested on one GPU/driver only; more AMD series and Windows results are needed.

## Reproduce

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON
cmake --build build -j

export EDEN_NX2_FORCE_MESH_FALLBACK=1
./build/bin/lichii
```

Open the game, right-click it and choose **Precache Shaders** if the warm files
are missing, then boot it and let the loading screen finish the warm-up.

Logs:

```bash
grep -E "Shader precache|Nx2Perf gpl|FORCE_MESH_FALLBACK" \
  ~/.local/share/lichii/log/eden_log.txt
```
