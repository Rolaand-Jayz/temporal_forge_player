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

Requirements (Linux): C++23 compiler, CMake ≥ 3.24 + Ninja, Vulkan loader +
headers, FFmpeg dev libs, Qt 6.6+ (Core/Gui/Quick/Qml/Widgets).

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

- **FSR 2.3 SDK** — stable production path (default).
- **FSR4-RE Experimental** — research path, opt-in only, never default.
- **Spatial fallback** — always-available reliability path.

If a backend fails, playback falls back to spatial scaling and continues
with a non-blocking warning.

## Status

Phased build per `spec 06`. See the spec pack for the full design.
