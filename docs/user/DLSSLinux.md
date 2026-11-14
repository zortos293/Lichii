# Native NVIDIA DLSS and neural rendering on Linux

Lichii can replace a supported game's upscaler with native Linux NVIDIA DLSS, use
DLAA at the game's render resolution, and optionally apply Standard, Natural or
Cinematic neural rendering before the game's HUD is composed.

Neural rendering uses the FP8 Vulkan backend from
[OpenDLSS-NR](https://github.com/maanHimself/OpenDLSS-NR), with model data imported
locally. Lichii does not bundle the model, DLSS5VKLayer, or a Wine helper.

## Requirements and files

- Linux x86_64, Lichii built with `ENABLE_NVIDIA_DLSS=ON`, and the Vulkan renderer.
- An NVIDIA GPU and driver supporting DLSS. The driver's Linux NGX component,
  `libnvidia-ngx.so.1`, must be installed and discoverable by the dynamic loader.
  Install it through your distribution's NVIDIA driver packages.
- Neural rendering additionally needs Vulkan 1.3 and the FP8/cooperative-matrix
  and CUDA-kernel-launch extensions checked at game startup. It has been tested
  on an RTX PRO 6000 Blackwell with Linux drivers 615.78.08 and 620.70.06. This is a tested
  configuration, not a minimum driver version or a promise of support on every
  RTX GPU. Ordinary DLSS remains usable when neural rendering is unsupported.

| Feature | File to obtain | How to install it |
| --- | --- | --- |
| DLSS super resolution and native DLAA | `libnvidia-ngx-dlss.so.310.9.16` | Use **Download NVIDIA DLSS…** in Graphics, or **Import Linux DLSS…**. |
| Optional neural rendering | `nvngx_dlssnr.dll`, model version **310.8.0** | Use **Import neural rendering model…** in Graphics. |

The DLSS download button retrieves the pinned runtime from the
[official NVIDIA DLSS repository](https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16)
after displaying NVIDIA's license, then verifies its SHA-256:

```text
30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6
```

For neural rendering, supply your own copy of the 310.8.0 DLL, for example from an
existing installation that provides it. If you already use DLSS5VKLayer, its
model file can be imported directly; its usual location is
`${XDG_DATA_HOME:-$HOME/.local/share}/dlssnr/binaries/nvngx_dlssnr.dll`.
The known-good source DLL used for testing has SHA-256:

```text
e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e
```

Use `sha256sum /path/to/nvngx_dlssnr.dll` to inspect your copy. The importer checks
the embedded model resource and packed weights; a matching filename or version
label alone is insufficient. It extracts data without loading or executing the
DLL. A Windows `nvngx_dlss.dll` is not the Linux DLSS runtime, and the
`nvngx_dlssnr.dll` model is a separate, optional file.

Wine, Proton, OptiScaler, DLSS5VKLayer, `nvngx.dll`, `nvapi64.dll`, and Streamline
DLLs are not required for this Linux path. No separate model extraction tool or
CUDA toolkit is required at runtime.

## Setup

1. Stop the game and open **Emulation → Configure → Graphics**.
2. Select **Vulkan** and the NVIDIA GPU. Enable **Use NVIDIA DLSS in place of a
   game's DLSS (experimental)**.
3. Click **Download NVIDIA DLSS…**, review the license, and accept the download.
   Alternatively, import the supported Linux `.so` listed above.
4. Set **NVIDIA DLSS quality** to **Automatic (game resolution)** for the game's normal
   output resolution, or **Native (DLAA)** to keep the game's render resolution.
   Quality, Balanced, Performance and Ultra Performance are also available. An
   explicit mode is used only when NVIDIA reports support for the game's input
   and output sizes; otherwise the backend selects a supported mode and logs the
   fallback. In most games these choices do not change the internal render resolution.
   Ocarina docked with the Max Resolution add-on is an exception: Balanced,
   Performance and Ultra Performance change its render scale when the game starts.
   Restart the game after changing that scale; live changes use the current scene size.
   For Ocarina's 1440p render mode, Native means **2560×1440 → 2560×1440**;
   it does not change the render resolution to match a 4K display.
5. For neural rendering, click **Import neural rendering model…** and select
   `nvngx_dlssnr.dll`. Choose **Standard**, **Natural**, or **Cinematic** in
   **DLSS neural rendering**. Leave it **Off** for ordinary DLSS/DLAA.
6. Apply the settings and start the game. While playing, reopen Graphics, change
   the **NVIDIA DLSS** checkbox, **NVIDIA DLSS quality**, **NVIDIA DLSS model preset**,
   **DLSS neural rendering**, or the neural controls
   below, and click **Apply** or **OK**. The runtime choices take effect without
   restarting and support per-game overrides; Ocarina's render-scale patch still
   requires a restart. If a per-game override is active,
   change it in the running game's
   configuration rather than the global settings.

You can switch between Off, Standard, Natural, and Cinematic in the same session.
Style changes reset neural history to avoid mixing the previous look into the new
one. Off releases the neural model's GPU resources while ordinary DLSS/DLAA
continues. Turning it back on or changing quality can briefly pause rendering
while model resources are prepared. The main NVIDIA DLSS checkbox can also be
switched during gameplay on Linux, including when the game started with it off.
Switches drain pending renderer work and reset temporal history. Install the
runtime files before starting the game; renderer and GPU selection still require
a restart. Windows retains its startup-only master checkbox.

**NVIDIA DLSS model preset** selects the SR/DLAA reconstruction model: M (the
DLSS 4.5 default), NVIDIA default, E, F, J, K or L. It is separate from the
optional DLSS neural-rendering model and its Standard/Natural/Cinematic styles.
Changing the reconstruction preset does not disable neural rendering. See the
[DLSS guide](DLSS.md) for preset tradeoffs. While successful evaluations are
occurring, the status bar and overlay show the evaluated quality and selected
preset, including a quality fallback when applicable.

Neural controls range from **0–100%** and default to **100%**, preserving the
existing appearance:

- **Graphics → Neural rendering strength:** blends ordinary DLSS/DLAA with the
  neural result. At 0%, the original DLSS/DLAA image is shown; 100% applies the full
  effect. The model continues running at low or zero strength, so GPU cost stays
  roughly the same. Select neural rendering **Off** to release its GPU resources.
- **Graphics → Advanced → Neural rendering tone:** adjusts the model's tonal
  treatment, including style-dependent lighting and colour changes.
- **Graphics → Advanced → Neural rendering structure:** adjusts the model's
  changes to scene detail and shapes. Lower values can help retain the original
  appearance.

All three controls apply to the selected neural style and support per-game
overrides. Tone and Structure changes reset neural history; Strength blends the
output while retaining history. None of these adjustments reloads the model.

Launch Lichii without an external DLSS5VKLayer wrapper so only the selected
Lichii processing is applied. If using a custom launcher, keep its existing
profile paths when switching builds so Lichii uses the intended saves and settings.

During supported gameplay, the Graphics panel should show **Native NGX active**
and an increasing successful-evaluation count. With a neural style selected,
it should also show **Native neural rendering active** and submitted frames.
Being installed or ready alone does not mean the game has reached a supported
upscaler pass.

Linux shares the Windows backend's recognized game passes and input contracts.
Fire Emblem's spatial workaround remains the default; its existing
`EDEN_NX2_DLSS_REPLACEMENT=dlss` override is still required to select DLSS.
This feature does not add frame generation or ray reconstruction.

## Installed locations and troubleshooting

The **Open folder** button opens the active profile's `ngx` directory. In a
default Linux profile this is `${XDG_DATA_HOME:-$HOME/.local/share}/lichii/ngx`:

```text
ngx/
  libnvidia-ngx-dlss.so.310.9.16
  LICENSE.txt                       # saved by the download flow
  nr/310.8.0/
    manifest.json
    model/packed.bin
```

The NR importer creates the manifest and packed weights itself. The source DLL
does not need to be copied into the application directory. Portable profiles and
launchers overriding XDG paths use their own data directory; use **Open folder**
to find the one currently in use. `LICHII_NGX_DIR` can override the Linux DLSS
runtime search directory, but does not relocate the imported neural model.

- **Missing NGX component:** check your distribution's NVIDIA driver installation
  for `libnvidia-ngx.so.1`; importing the DLSS model does not install this driver
  component.
- **Model import rejected:** use the supported 310.8.0 model. If a previous import
  is damaged or interrupted, close Lichii and move the `nr/310.8.0` or
  `nr/310.8.0.importing` directory aside before importing again.
- **Neural rendering unavailable or out of memory:** the panel/log reports the
  reason and ordinary NVIDIA DLSS continues. Free GPU memory or select a
  compatible GPU/driver, then restart the game.
- **No successful evaluations:** check the selected GPU, the replacement setting,
  and whether the game has reached a supported pass. This integration is not a
  universal post-processing layer for every game.

## Building

Follow [the normal build instructions](../Build.md). Native DLSS is enabled by
default on supported Linux x86_64 builds; it can be selected explicitly with
`-DENABLE_NVIDIA_DLSS=ON`, or omitted with `-DENABLE_NVIDIA_DLSS=OFF`.

The native neural backend needs Python 3 and glslang 16.6.0 or newer to generate
its embedded shaders/PTX during the build. If a suitable glslang executable is
not found, CMake downloads and builds a pinned, checksum-verified glslang 16.6.0.
Four pinned NVIDIA interface headers are fetched and checksum-verified at
configure time. For offline builds, `-DLICHII_NGX_SDK=/path/to/sdk` can supply them
from that checkout's `include` directory; glslang and the other build dependencies
must also be available locally. No NVIDIA static library is linked, and model
files are obtained separately at runtime as described above.

## Developer checks

`EDEN_NX2_DLSS_LOG=1` adds NGX diagnostics. Runtime lookup checks `LICHII_NGX_DIR`,
then the profile's `ngx` folder, then `ngx` beside the executable. Each location
accepts the versioned runtime filename or `libnvidia-ngx-dlss.so`; the in-app
import/download flow verifies the pinned runtime. NGX caches go under `ngx` in
Lichii's cache directory.

With `LICHII_TESTS=ON`, the optional GPU smoke test can be run without booting a game:

```sh
cmake --build build --target ngx_smoke
build/bin/ngx_smoke /path/to/linux/dlss/runtime/directory 0 1
# Also exercise neural rendering with an already imported model:
build/bin/ngx_smoke /path/to/linux/dlss/runtime/directory 0 1 /path/to/ngx/nr/310.8.0
```

The numeric arguments are the Vulkan GPU index and image scale (1–8). Scale 8
uses 2560×1440 input and 3840×2160 upscaled output. The test covers device
lifecycle (including initially disabled DLSS), SR/DLAA readback, reset, resize,
HDR/depth modes, padded depth images with a smaller render region, motion-view
rebinding, live quality/preset/status changes,
style/Off changes, strength blending (including zero-strength HDR),
tone/structure history resets, DLSS initialization before neural model allocation,
and injected NGX failure. Use `missing` instead of
the NR model path to check missing-model fallback. It is excluded from `ctest`
because it requires the GPU and separately supplied models. Disable external
graphics layers for this test.

The native NR kernel watchdog is checked during initialization and motion-view
rebinding; continuous runtime timeout monitoring is not implemented. RenderDoc
1.46 filters required NVIDIA extensions, so it could not capture this native
DLSS/NR path. Synthetic validation and gameplay checks do not establish support
for every GPU or game.

The backend's pinned sources and local adaptations are recorded in
[`externals/opendlss-nr/UPSTREAM.json`](../../externals/opendlss-nr/UPSTREAM.json).
The NGX loader uses the driver core ABI documented by the declarations in
`ngx_api.h`, including entry points that differ from the static SDK wrappers.
