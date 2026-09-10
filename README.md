# Temporal Forge Player

> **Experimental research project · Flagship portfolio work**

Temporal Forge adapts temporal reconstruction/upscaling concepts associated with AMD FSR to ordinary decoded video. It is not frame interpolation or frame generation:

`one decoded video frame → one reconstructed/upscaled displayed frame`

The source frame count, timestamps, and source cadence remain the contract. A high-refresh display may repeat a reconstructed frame; the player does not invent intermediate frames.

## Why this is difficult

A native game renderer can provide motion, jitter, reset/history context, exposure, reactive/composition signals, and other semantic inputs designed for temporal reconstruction. Finished video does not naturally preserve those signals in the same form. Temporal Forge therefore treats reconstruction quality as a research problem: determine what can be recovered or synthesized from video, what cannot be recovered reliably, and which inputs actually change temporal behavior.

## Current status

The player has an operational GPU-native pipeline with stable FSR 2.3, an opt-in experimental FSR4 reverse-engineering path, and a spatial fallback. The project remains experimental research; it does not claim production readiness or parity with AMD's implementation. Meaningful current research may live on non-default branches—see the branch history and campaign documentation before treating experimental behavior as part of `main`.

The remaining problem is reconstruction quality, not merely moving frames through a pipeline. Current work focuses on expected-input semantics, motion transfer, temporal history, jitter, exposure, masking, reset behavior, and causal diagnostics.

## What this project demonstrates

- Native C++/Vulkan/FFmpeg/Qt integration on Linux
- GPU video processing and temporal reconstruction
- Reverse-engineering-informed interoperability research
- Reproducible experiments with benchmark conditions and provenance
- Explicit separation of measured facts, inferences, hypotheses, and unobserved behavior
- Negative-result preservation, independent review, adversarial challenge, remediation loops, and verification gates

The methodology matured here from earlier AMD-first application and reverse-engineering work. Those projects are part of the lineage; they should not be read as though this formal process existed from the beginning.

## Documentation map

Start with the repository's current-state and architecture documents, then descend into the active research campaign, benchmark evidence, technical decisions, and archived/superseded reports. The README below preserves the build and backend details; authoritative documentation takes precedence where a dated experiment and current implementation differ.

## Build

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
