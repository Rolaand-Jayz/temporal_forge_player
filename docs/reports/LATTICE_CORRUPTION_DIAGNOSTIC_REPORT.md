# Lattice corruption diagnostic — 2026-09-03

The bounded diagnostic was run against `sintel_cave`, frame 48, with CAS disabled and the campaign's AMD baseline controls. Existing campaign artifacts were not modified. Raw outputs and stage captures are in the run recorded by `benchmarks/quality_sweeps/lattice_corruption_diagnostic/LATEST_RUN`.

## Stage result

The source-model capture is identical for the bad GPU-prefilter and reference-resize arms (`1280x720`, frame 48, fingerprint `0x302b82493cc7edbe`). Therefore the corruption is not introduced by decoded input/YUV→RGB10 conversion (stage A). The model-color capture changes only after the source→model resize (`960x540`): GPU `0xc52258e1b800b429`; reference `0x3d53dbdac80eb1a2`. The healthy 360→720 control has no source/model resize and remains free of the campaign-observed lattice.

The first implicated stage is **B: the source→model prefilter output**. The reference-resize ablation improves whole-scene SSIM from `0.692721` to `0.716862` and PSNR from `30.573677` to `30.828827`, while preserving the same downstream configuration. This is evidence that the custom GPU prefilter contributes materially to the defect.

The smallest evidence-supported production change is now in the Vulkan prefilter itself: its sampling footprint is scale-aware. Upsampling retains the native Catmull–Rom footprint; downsampling scales the separable kernel support and weights by the destination/source ratio, avoiding the old fixed 4×4 footprint at rational shrink phases. The GPU path remains the default; the libswscale branch is still opt-in diagnostic-only. A rerun of the bad geometry changed the Stage-B fingerprint and reduced the normalized 2×2 periodic-energy tripwire from approximately `0.0967` to `0.0802` (reference resize: `0.0815`). This is a bounded regression guard, not a claim of visual approval; human review remains required.

The required visual audit then falsified the stronger hypothesis that Stage-B correction alone removes the visible defect: both the corrected GPU output and the reference-resize output still show the repeating lattice in the dragon flank and foreground rock, while the 360→720 control does not. A controlled bad-case capture with best-findings temporal feedback, color history, and recurrent feedback disabled removes the lattice. This places the remaining P0 trigger downstream of Stage B, in the temporal feedback/graph handoff for source/model-mismatched geometry. The scale-aware prefilter remains a valid Stage-B correction, but is not yet sufficient for Definition-of-Done visual cleanliness.

## Captured stages

- `stage-A-sourceModel.ppm`: post YUV→RGB10 source image.
- `stage-B-color.ppm`: post source→model resize image.
- `stage-C-prepass.f32` plus JSON metadata: prepass readback probe.

The runner records exact geometry, environment, player revision, logs, metrics, and per-file artifacts in a fresh, non-overwriting run directory.

## Resolution

No CAS or motion behavior was changed. The ablation remains an oracle only; replacing the Vulkan prefilter with a synchronous CPU readback/resize would violate the project's fast native path. The bounded diagnostic, shared capture primitives, scale-aware GPU correction, stage-capture contract, and automated periodic-energy tripwire are complete. The downstream temporal trigger still requires a focused root-cause fix and a fresh visual qualification before treating the corrected baseline as campaign-approved.
