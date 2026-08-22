# Temporal Forge developer map

This repository is intentionally documented for a developer who is joining in
the middle of an experiment. The comments in the implementation are the local
truth for ownership and data flow; this file is the map that connects those
comments across modules.

## The runtime path

```text
Qt/QML controls
    -> PlaybackEngine (threading, clocks, playlist, frame lifetime)
    -> Demuxer + VideoDecoder (FFmpeg packets and decoded frames)
    -> SideBufferSynth (motion, depth, reactive, exposure inputs)
    -> GpuImageUploader (CPU/DRM frame -> Vulkan images)
    -> BackendSelector (INT8 -> FSR fallback -> spatial fallback)
    -> Fsr4DispatchHarness (prepass -> convolution/scatter -> postpass -> SPD)
    -> VideoSurfaceItem (Qt Quick texture/presentation)
    -> QML overlays and controls
```

The reverse dependencies matter too: `VideoSurfaceItem` pulls the current
image from `PlaybackEngine`, the playback thread owns decode and dispatch
ordering, and the render thread owns presentation. A change that crosses that
boundary must preserve the existing hand-off and lifetime comments in the
relevant header.

## Where a feature belongs

- Playback policy, frame pacing, seeking, history resets, or thread ownership:
  `src/core/PlaybackEngine.*`.
- Container/packet stream discovery: `src/media/Demuxer.*`.
- Codec frames, timestamps, hardware frames, and motion-vector extraction:
  `src/media/VideoDecoder.*`.
- Motion/depth/reactive/exposure side inputs: `src/render/SideBufferSynth.*`.
- Vulkan image allocation, upload, readback, history ping-pong, and presentation
  scaling: `src/render/GpuImageUploader.*`.
- Backend selection and fail-closed fallback: `src/backend/BackendSelector.*`.
- Neural dispatch resources and pass ordering: `src/render/Fsr4DispatchHarness.*`
  and `src/render/fsr4/Fsr4ConvSteps.*`.
- Persistent user settings and quality-lab overrides:
  `src/config/SettingsStore.*` and `src/config/QualityLabConfig.*`.
- QML-facing state and labels: `src/ui/*` and `resources/qml/*`.
- GPU algorithm behavior: `shaders/fsr4/*`. A shader comment must name its
  descriptor inputs, output image, dispatch geometry, and the C++ caller that
  binds it.
- Reproducible captures and comparison data: `benchmarks/video_corpus/*` and
  `benchmarks/quality_sweeps/*`.
- Distributable human review UI: `tools/build_review_harness.mjs` and
  `tools/embed_review_harness.mjs`. It consumes result metadata; it must not
  change reconstruction behavior.

## Comment contract

Every maintained source file should have a file comment. Every meaningful
class, struct, enum, shader stage, QML object, script function, and public or
cross-module function should explain four things in plain language:

1. What it does.
2. Why it exists and what invariant it protects.
3. What it consumes and produces.
4. Who calls it and what downstream code depends on it.

Comments must describe the real current behavior. If a path is experimental,
fallback-only, capture-only, or unavailable on the current machine, say so.
Do not describe a planned feature as if it already works. If behavior changes,
update the nearby comment in the same patch.

## Build and test lanes

- `build-fast/` is the active Release/Ninja build used by quality scripts.
- `build/` is a separate Debug build for local diagnosis.
- `benchmarks/video_corpus/run_quality.sh` and the quality sweep scripts are
  capture workflows; they can produce large ignored media under `results/`.
- `tools/build_review_harness.mjs` creates the standalone reviewer, and
  `tools/embed_review_harness.mjs` creates the single-file shareable reviewer.
- `external/` is vendored or imported dependency material. Do not rewrite it
  to add local commentary; document the integration boundary in our files.

## Safe change checklist

Before changing a pipeline stage, trace both directions: identify the caller
that supplies the inputs and every consumer of the outputs. Then check the
thread/queue boundary, image format/layout, dimensions, history lifetime, and
fallback behavior. Build with `build-fast`, run focused tests, and only then
run a capture. Never use a passing compile as evidence that image quality or
GPU execution was validated.
