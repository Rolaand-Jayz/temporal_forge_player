# Synthetic video jitter contract

Synthetic jitter is a separate color-input operation. Motion estimation reads
the original decoded frame pair before this operation and never receives the
jitter displacement.

`SideBufferSynth` generates Halton(2,3) offsets in render/source-pixel units.
The phase count now follows the FidelityFX helper rule used by the checked-in
SDK: `ceil(8 * (displayWidth / renderWidth)^2)`, clamped to at least one
phase. The phase advances only from the decode/FSR frame loop and resets when
the side-buffer reset path is entered or either render/presentation size
changes.

`GpuImageUploader::setInputJitter` transfers that exact offset to the YUV and
DRM conversion push constants. The conversion shader applies a clamp-to-edge
bilinear luma/chroma sample shift while constructing the FSR color input; this
is the single pre-FSR resampling stage. The same jitter values are passed to
`Fsr4DispatchHarness::FrameDispatchInput` for the temporal prepass. Motion
vectors remain unjittered, and the existing motion-jitter cancellation flag is
not enabled.

Future-aligned/interpolated diagnostic modes explicitly replace synthetic
jitter with their own sample, so the upload pass sends zero jitter and the FSR
constants report zero jitter together. This prevents a reported offset from
describing a color sample that was never applied.

The implementation is an approximation for prerecorded video: it supplies
different subpixel sampling phases but cannot recreate renderer-level scene
samples that were discarded before encoding.
