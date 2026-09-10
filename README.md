# Temporal Forge Player

> **Experimental research project · Flagship portfolio work**

Temporal Forge investigates adapting **AMD FSR 4.1-style temporal reconstruction/upscaling to ordinary decoded video**. It is not frame interpolation or frame generation:

`one decoded video frame → one reconstructed/upscaled displayed frame`

The source frame count, timestamps, and cadence remain the contract. A high-refresh display may repeat a reconstructed frame; the player does not invent intermediate frames.

## Why this is difficult

A native game renderer can provide motion, jitter, reset/history context, exposure, reactive/composition signals, and other semantic inputs designed for temporal reconstruction. Finished video does not naturally preserve those signals in the same form.

Temporal Forge therefore treats reconstruction quality as a research problem: determine what can be recovered or synthesized from video, what cannot be reconstructed reliably, and which inputs actually change temporal behavior.

## Current status

The player has an operational GPU-native pipeline with:

- **FSR 2.3 SDK** — stable default temporal path
- **FSR4-RE Experimental** — opt-in FSR 4.1 reverse-engineering-informed research path
- **Spatial fallback** — reliability path when a temporal backend cannot run

The project remains experimental research and does not claim production readiness or parity with AMD's implementation. Meaningful current research may live on non-default branches; consult branch history and campaign documentation before treating experimental behavior as part of `main`.

The remaining problem is reconstruction quality, not merely moving frames through a pipeline. Current work focuses on expected-input semantics, motion transfer, temporal history, jitter, exposure, masking, reset behavior, composition, and causal diagnostics.

## What this project demonstrates

- Native C++23 / Vulkan / FFmpeg / Qt integration on Linux
- GPU video processing and temporal reconstruction
- FSR 4.1 reverse-engineering-informed interoperability research
- Reproducible experiments with benchmark conditions and provenance
- Explicit separation of measured facts, inferences, hypotheses, and unobserved behavior
- Preservation of negative results when they explain system behavior
- Independent review, adversarial challenge, remediation loops, and verification gates

The methodology matured here from earlier AMD-first application and reverse-engineering work. Those projects are part of the lineage; they should not be read as though this formal process existed from the beginning.

## Core rule

```text
1 decoded input frame → 1 reconstructed/upscaled displayed output frame
same timestamps · same frame count · same source frame rate
```

The player reconstructs each source frame at a higher internal resolution through the selected temporal path, then scales that result to the current window or fullscreen surface. Window resizing changes presentation only; it does not redefine the source cadence contract.

## Scaling model

```text
source frame
  → temporal reconstruction target (source × preset ratio)
  → final presentation scale to window
```

The reconstruction target depends on source size and preset, not window size. Resizing the window changes only presentation scaling and should not recreate the temporal context or reset history.

| Preset | Ratio |
|---|---:|
| NativeAA | 1.0x |
| Quality | 1.5x |
| Balanced | 1.7x |
| Performance | 2.0x |
| Ultra Performance | 3.0x |

## Documentation map

Start with the repository's current-state and architecture documents, then descend into the active research campaign, benchmark evidence, technical decisions, and archived or superseded reports. Authoritative dated experiment documentation takes precedence when historical reports differ from the current implementation.

Useful entry points include:

- [`docs/README.md`](docs/README.md) — documentation authority map
- [`docs/FSR4_RE_STATUS.md`](docs/FSR4_RE_STATUS.md) — dated FSR 4.1 RE reconstruction status/history
- [`benchmarks/quality_sweeps/`](benchmarks/quality_sweeps/) — current quality and causal experiment tooling/evidence

## Build

Requirements on Linux: C++23 compiler, CMake ≥ 3.24, Ninja, Vulkan loader and headers, FFmpeg development libraries, and Qt 6.6+ (Core/Gui/Quick/Qml/Widgets).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/temporal_forge_player
```

Set `TFORGE_VK_VALIDATE=1` to enable the Vulkan validation layer.

## Reliability behavior

If a selected experimental backend cannot initialize or execute safely, playback can fall back to spatial scaling with a non-blocking warning rather than silently presenting the experimental path as successful.

## Research status

Temporal Forge is an active experimental R&D project. Its purpose is not to claim that finished video supplies the same information as a game renderer; it is to determine, through controlled experiments, which missing temporal inputs matter, which useful surrogates can be synthesized, and where the approach reaches a hard information boundary.
