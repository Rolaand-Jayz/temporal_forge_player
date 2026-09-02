# Temporal Forge motion-vector contract

This document records the contract discovered in the current player before the
cheap motion-estimator work. It is deliberately separate from the FSR shader
implementation: the estimator adapts to this contract, rather than changing
the temporal reconstruction semantics.

## Ownership and data flow

1. `VideoDecoder::receiveFrame` owns FFmpeg side-data extraction. The decoder
   enables `AV_CODEC_FLAG2_EXPORT_MVS` before `avcodec_open2`, reads
   `AV_FRAME_DATA_MOTION_VECTORS`, and converts `motion_x/motion_y` with
   `motion_scale` into source-pixel floats.
2. `DecodedVideoFrame::motionVectors` carries the sparse vectors and preserves
   FFmpeg's signed `source` reference-list value. Positive values are future
   references and are not valid causal history by themselves.
3. `PlaybackEngine` filters causal vectors, optionally applies bounded motion
   analysis, scales only block coverage into the neural model grid, and passes
   the vectors to `GpuImageUploader::uploadMotion`.
4. `GpuImageUploader` uploads compact block records and runs
   `shaders/fsr4/codec_motion_expand.comp`. The shader deterministically stamps
   accepted blocks into an `RG16F` motion image and an `R8` coverage/confidence
   image.
5. `Fsr4DispatchHarness::recordPrepass` binds those images. The prepass samples
   motion in model coordinates, scales the displacement once from source/render
   pixels to output pixels, and reprojects the previous history image.

## Canonical convention

Internally, a vector means **current displayed-frame pixel to the location of
that pixel in the previous displayed frame**. `mvX` and `mvY` are source/render
pixel displacements. They are not normalized UVs and are not output-pixel
units. The current decoder adapter is the only boundary that interprets
FFmpeg's motion-vector fields; downstream code must not add another sign
conversion.

The sparse block origin and extent are initially in source/render pixels. The
`scaleMotionCoverageToModel` adapter changes origin, extent, and displacement
to the model-grid coordinates used by the `RG16F` texture. The prepass then
applies only the model-to-output scale when sampling output-space history;
neither stage negates or reinterprets the vector. This is the single documented
resolution-conversion boundary for the FSR-facing path. The earlier wording
that said displacement remained in source pixels was stale and contradicted
the live adapter at `PlaybackEngine.cpp`.

The current path keeps motion unjittered. Jitter is passed separately in the
prepass constants, and any jitter correction is an explicit diagnostic mode.
Synthetic jitter changes the color sampling phase and is reported to FSR only
when that shifted sample was actually produced. It cannot create new spatial
information from a finished video frame, so zero-jitter remains a required
control and a viable production fallback; it is not silently treated as an
equivalent to renderer-generated jitter.
The first displayed frame, a seek/discontinuity, scene cut, invalid reference,
or low-confidence vector must not reuse old history.

When the combined quality profile is active, an FFmpeg frame that has motion
metadata but no usable `source == -1` entry may use the estimator's robust
global translation fallback. That fallback is tiled across the source frame,
marked with `source == -1`, and capped at low confidence. The ordinary
baseline intentionally keeps the older empty-field behavior so this recovery
can be measured independently and cannot silently change baseline captures.

## Estimator contract

The cheap estimator receives the current and previous analysis luma images plus
the filtered codec block seeds. Its output remains in the same sparse
source-pixel convention above. It may add confidence and conservative fallback
vectors, but it must not change FSR resource bindings, motion scale semantics,
jitter semantics, or history reprojection code.

The intended stages are: compact codec seeds, robust global fallback, reduced
resolution local search, residual confidence, scene-cut rejection, and
edge-aware dense reconstruction. GPU work owns rasterization/reconstruction;
the CPU prepares only compact metadata. Future or ambiguous codec references
are retained only as low-confidence diagnostics and are never silently treated
as the immediately previous displayed frame.

## Evidence sources

- `src/media/VideoDecoder.cpp`: FFmpeg export and `AVMotionVector` conversion.
- `src/media/VideoDecoder.hpp`: `MvEntry` and decoded-frame ownership.
- `src/core/PlaybackEngine.cpp`: causal filtering, model coverage scaling, and
  dispatch handoff.
- `src/render/GpuImageUploader.cpp`: sparse-vector upload and GPU expansion.
- `shaders/fsr4/codec_motion_expand.comp`: deterministic dense texture write.
- `shaders/fsr4/prepass_pq_eotf.comp`: motion sampling and history reprojection.
- `src/render/Fsr4DispatchHarness.hpp`: image formats and dispatch inputs.
