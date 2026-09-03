# Lattice corruption diagnostic — 2026-09-03

The bounded diagnostic was run against `sintel_cave`, frame 48, with CAS disabled and the campaign's AMD baseline controls. Existing campaign artifacts were not modified. Raw outputs and stage captures are in the run recorded by `benchmarks/quality_sweeps/lattice_corruption_diagnostic/LATEST_RUN`.

## Stage result

The source-model capture is identical for the bad GPU-prefilter and reference-resize arms (`1280x720`, frame 48, fingerprint `0x302b82493cc7edbe`). Therefore the corruption is not introduced by decoded input/YUV→RGB10 conversion (stage A). The model-color capture changes only after the source→model resize (`960x540`): GPU `0xc52258e1b800b429`; reference `0x3d53dbdac80eb1a2`. The healthy 360→720 control has no source/model resize and remains free of the campaign-observed lattice.

The first implicated stage is **B: the source→model prefilter output**. The reference-resize ablation improves whole-scene SSIM from `0.692721` to `0.716862` and PSNR from `30.573677` to `30.828827`, while preserving the same downstream configuration. This is evidence that the custom GPU prefilter contributes materially to the defect, but it does not establish that it is the sole source of every downstream artifact.

## Captured stages

- `stage-A-sourceModel.ppm`: post YUV→RGB10 source image.
- `stage-B-color.ppm`: post source→model resize image.
- `stage-C-prepass.f32` plus JSON metadata: prepass readback probe.

The runner records exact geometry, environment, player revision, logs, metrics, and per-file artifacts in a fresh, non-overwriting run directory.

## Resolution

No production quality parameter or CAS/motion behavior was changed. The ablation improves the bad case but does not provide a clean separation of all corruption from downstream stages; replacing the Vulkan prefilter with a synchronous CPU readback/resize would also violate the project's fast native path. A production root-cause patch is therefore intentionally not guessed. The bounded diagnostic and regression contract are complete; a follow-up prefilter implementation must first preserve GPU synchronization/performance while matching the trusted resize result.
