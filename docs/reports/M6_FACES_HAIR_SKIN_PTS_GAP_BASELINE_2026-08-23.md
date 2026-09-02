# M6 faces-hair-skin temporal baseline

This slice contains exactly one baseline row in
`benchmarks/quality_sweeps/m6_temporal_faces_hair_skin_baseline.json`:

`base_only_bilinear / tos_daylight / faces-hair-skin`

The sequence is real Tears of Steel corpus content, source frames 42–59, with
the grounded frame-48 face annotation carried into an explicit 1920×1080 mask
(x=250, y=100, width=800, height=700). The static metrics use captured frames
0–8, before the event at captured frame 9; the event metrics use the full
18-frame event-spanning sequence with eight frames of pre/post context.

The fresh runtime trace selected a PTS gap as the authoritative cause:

- event index 9, PTS 875000 us;
- PTS delta 541.666992 ms versus an expected 29.7092056 ms interval;
- threshold rule `ptsGapMs > expectedFrameIntervalMs * 2.5`;
- histogram delta 0.0569714904 and motion confidence 0.737332225 did not
  independently trigger their thresholds;
- raw runtime labels remain `detectorSceneCut=true`,
  `resetCause=detector_scene_cut`, and `ghostCause=detector_scene_cut`.

Measured metrics are `static_flicker=0.013911833`,
`edge_variance=0.000050430`, `motion_compensated_error=0.015225080`,
`ghost_duration_frames=0`, and `reset_recovery_frames=0`. The zero values are
measured outputs from explicit event thresholds, not unavailable values or
fabric-event substitutions.

The capture retained its event trace, causal codec-motion sidecar, static mask,
metrics CSV, and full frame/reference artifacts under
`/tmp/tforge-m6-face-event-20260823/`. Any failed attempt would be preserved by
the runner under its `failure-retained/` directory; no failure was used to
manufacture this row. No reconstruction, shader, model, image-quality, or
benchmark-image source was edited for this slice.
