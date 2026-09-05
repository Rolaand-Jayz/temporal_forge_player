# Architecture

Temporal Forge Player is a GPU-native video player that applies FSR-style
temporal upscaling to local video files **without** frame interpolation, frame
generation, or cadence conversion.

```text
1 decoded input frame → 1 upscaled displayed output frame
same timestamps · same frame count · same source frame rate
```

This document is the source of truth for the layered structure, the threading
model, and the backend cascade. Read it before moving code across files or
touching the playback/FSR4 lifecycle.

## Layer diagram

```mermaid
flowchart TD
    UI["ui/<br/>QML + QQuickItem + controllers"] --> CORE["core/<br/>PlaybackEngine orchestration"]
    CORE --> MEDIA["media/<br/>FFmpeg demux + decode"]
    CORE --> BACKEND["backend/<br/>upscaler selection + cascade"]
    CORE --> RENDER["render/<br/>Vulkan + FSR4 dispatch + upload"]
    CORE --> AUDIO["audio/<br/>miniaudio sink + ring buffer"]
    CONFIG["config/<br/>settings persistence"] --> UI
    UTIL["util/<br/>math, jitter, alignment, log"] --> CORE
    UTIL --> RENDER
    UTIL --> BACKEND
```

### `media/` — FFmpeg pipeline

- `Demuxer` — opens a URL, reads packets, exposes stream info + seeking.
- `VideoDecoder` — decodes video packets (VAAPI when available, CPU fallback).
  Produces `DecodedVideoFrame` with YUV planes + motion vectors.
- `AudioDecoder` — decodes audio packets to interleaved float samples.

### `core/` — orchestration

- `PlaybackEngine` (QObject, exposed to QML as `playback`) owns the demux,
  decode, and audio threads plus the FSR4 dispatch path. This is the load-bearing
  class; see **Threading model** below.

### `backend/` — upscaler cascade

A single interface (`ITemporalUpscalerBackend`) with multiple implementations
selected by `BackendSelector`. On failure the selector cascades down so
playback never silently breaks:

```text
1. FSR4-RE INT8 experimental   (proof-gated default selection on supported RDNA3)
        ↓ fails
2. FSR 3.1.5 fallback           (real temporal upscaler from open SDK)
        ↓ SDK not linked / fails
3. Spatial fallback             (always-available reliability path)
```

### `render/` — Vulkan

- `VulkanContext` — instance/device/queue/command-pool lifecycle. Shares the
  Vulkan instance with Qt so presentation and compute use the same device.
- `GpuImageUploader` — uploads decoded frames to GPU images (DRM-prime import
  when available, CPU staging fallback), manages staging buffers + readback.
- `Fsr4DispatchHarness` — creates the FSR4 compute pipelines + descriptor sets
  and runs the per-frame INT8/FP16 dispatch (the 14-pass U-Net CNN).
- `SideBufferSynth` — synthesizes reactive/depth masks for the FSR side inputs.

### `audio/` — master clock

- `RingBuffer` (`AudioRing`) — single-producer/single-consumer lock-free ring.
- `AudioSink` — miniaudio-backed output. **The audio device callback is the
  master clock** (spec 01). Never slow audio to wait for video.

### `ui/` — QML surface

- `VideoSurfaceItem` — native Vulkan `QQuickItem` that samples the completed
  FSR4 output image directly on the Qt render thread.
- Controllers (`FsrController`, `VideoSettingsController`, `CompareController`,
  `PostProcessController`, `ScreenCaptureController`, `DebugOverlayModel`) are
  QML bridges bound from `main.cpp` as context properties.

## Threading model (spec 01)

There are four threads that interact. Getting this wrong has caused two
recorded regressions (see repo notes). **Do not change these invariants
without a test that proves the new behavior.**

| Thread | Owner | Role |
|---|---|---|
| Main / UI | Qt | window events, controls, settings, QML |
| Playback / decode | `PlaybackEngine` | demux scheduling, video+audio decode, FSR4 dispatch |
| Audio device (realtime) | miniaudio | the master clock — reads the ring buffer |
| Qt render | Qt scene graph | samples the FSR4 output image for display (~60Hz) |

### Load-bearing invariants

1. **`fsrDispatchMutex_` serializes UI-thread teardown vs decode-thread dispatch
   ONLY.** The render-thread accessors (`fsr4NativeOutput()` / `fsr4RawOutput()`)
   must **never** take this mutex. Locking them serialized every 60Hz render
   frame against every 5ms FSR4 dispatch and caused visible stutter.
2. **Teardown safety for the render thread comes from `vkQueueWaitIdle()`** in
   `teardownFsr4Path()`, which retires in-flight GPU work before the uploader is
   freed. The narrow CPU pointer-read race (TOCTOU on `fsr4Uploader_`) is
   pre-existing and acceptable — it only happens on user-initiated teardown.
3. **`setFsrViewport()` only flips `fsr4Ready_` when the preset *scale* changes**,
   not on pure window resize. Resizing the window changes presentation only; it
   must never recreate the FSR4 context (that forces a multi-second weight-blob
   reload).
4. **The decode thread does synchronous `vkWaitForFences(UINT64_MAX)`** per frame
   inside `Fsr4DispatchHarness::dispatchFrame`. Anything that wants to stop the
   decode thread cleanly must let the current dispatch finish (or guard it with
   the abort flag `fsrAbortRequested_`).

### QML enum gotcha

An enum on a C++ class exposed via `setContextProperty` (an *instance*, not a
type) cannot be referenced as `TypeName.EnumValue` from QML — use raw integer
literals. Enums on classes registered via
`qmlRegisterType<Type>("URI", 1, 0, "QmlName")` **are** accessible as
`QmlName.EnumValue`.

## FSR target model (spec 02)

```text
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

## Where to look

- Lifecycle orchestration: `src/core/PlaybackEngine.cpp`
- FSR4 dispatch (the 14-pass CNN): `src/render/Fsr4DispatchHarness.cpp`
- Backend cascade: `src/backend/BackendSelector.cpp`
- Frame upload / DRM import: `src/render/GpuImageUploader.cpp`
- Audio clock: `src/audio/AudioSink.cpp`, `src/audio/RingBuffer.hpp`
- QML wiring: `src/main.cpp`, `resources/qml/`

For the reverse-engineering background on the FSR4 neural network, see
`docs/reports/FSR4_RECONSTRUCTION_STATUS_20260709.md`.
