# Temporal Forge Player

A GPU-native video player that applies FSR-style temporal upscaling to local
video files **without** frame interpolation, frame generation, or cadence
conversion.

## Core rule

```
1 decoded input frame → 1 upscaled displayed output frame
same timestamps · same frame count · same source frame rate
```

The player reconstructs each source frame at a higher internal resolution
via FSR, then scales that FSR output to the current window or fullscreen
surface. A high-refresh monitor may repeat an upscaled frame across
refreshes — that is presentation repeat, never a generated frame.

## Build

See the Requirements table below.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/temporal_forge_player
```

Set `TFORGE_VK_VALIDATE=1` to enable the Vulkan validation layer.

## Scaling model

```
source frame
  → FSR preset reconstruction target   (source × preset ratio)
  → final presentation scale to window
```

The FSR target depends only on source size and preset — **never** on window
size. Resizing the window changes only the presentation scale and never
recreates the FSR context or resets history.

| Preset | Ratio |
|---|---:|
| NativeAA | 1.0x |
| Quality | 1.5x |
| Balanced | 1.7x |
| Performance | 2.0x |
| Ultra Performance | 3.0x |

## Backends

- **FSR4-RE Experimental (default)** — INT8 reconstruction, proof-gated, the
  default backend selection on supported RDNA3 hardware (`SettingsStore`
  default + `allowExperimentalAsDefault = true`), with fallback on failure.
- **FSR 3.1.5 (SDK)** — compiled out in this tree: `TFORGE_HAVE_FSR3_SDK` is
  never defined by any build file, so this tier reports "SDK not linked" and
  is skipped in every build. It exists in source only as a stub for a future
  SDK integration.
- **Spatial fallback** — always-available compute-shader EASU/RCAS path once
  Vulkan initializes; the reliability floor.

Selection order (see `src/backend/BackendSelector.cpp`): FSR4-RE INT8 first,
then the SDK tier (currently unavailable), then spatial. If a backend fails,
playback falls back to spatial scaling and continues with a non-blocking
warning.

FSR4 native INT8 packs and generic weight blobs are **not redistributable
in-tree**. Provision them as described in
[`resources/fsr4/native_i8/README.md`](resources/fsr4/native_i8/README.md)
and [`tools/build_native_int8_pack.sh`](tools/build_native_int8_pack.sh).
At runtime the engine looks for native packs next to the executable
(`<exe_dir>/../resources/fsr4`, then `./resources/fsr4`) and for generic
weight blobs under `$TFORGE_FSR4_RE_ROOT` or
`$XDG_DATA_HOME/temporal-forge-player/fsr4/` (see
[`docs/reference/environment.md`](docs/reference/environment.md)). When the
assets are absent, the engine falls back down the backend chain and logs the
searched locations.

## Requirements

The runtime requires a **Vulkan 1.3** driver (instance created with
`apiVersion = VK_API_VERSION_1_3`). A Vulkan 1.2-only machine cannot run the
player.

| Package | Role | Required? | Behavior when missing |
|---|---|---|---|
| C++23 compiler (GCC 13+/Clang 17+ class) | build | required | configure/build fails |
| CMake ≥ 3.24 | build | required | configure fails |
| Ninja | build | required (hard configure requirement, `find_program(... REQUIRED)`) | configure fails |
| Qt 6.6+ — Core, Gui, Quick, Qml, Widgets, **ShaderTools** | build + runtime | required | configure fails (ShaderTools included in the required component list) |
| glslangValidator | build | required (`cmake/ShaderCompile.cmake`) | configure fails |
| Vulkan loader + headers, API 1.3 | build + runtime | required | build fails / runtime cannot start |
| FFmpeg dev libraries | build + runtime | required | configure/build fails |
| python3 | build tooling | required | tooling steps fail |
| miniaudio (vendored single header, v0.11.25, `external/`) | build | bundled | none — tracked in-tree |
| Vulkan headers shim (`external/vulkan_include/`) | build | bundled | none — tracked in-tree |
| ffmpeg executable with libx264 + aac encoders | test only | optional | `sample.mp4` fixture generation skipped; external-data tests SKIP, not FAIL |
| jq, magick (ImageMagick) | research/capture scripts | optional | affected scripts fail with a clear error |
| dxc + spirv-tools | native pack builds | optional | `tools/build_native_int8_pack.sh` cannot run |
| FidelityFX SDK (`TFORGE_ENABLE_FSR1_PROBE=ON`, default OFF) | optional probe | optional | probe target not built |

## Status

Phased build per `spec 06`. See [`docs/README.md`](docs/README.md) for current
architecture, quality work, reports, research, and archived plans.
