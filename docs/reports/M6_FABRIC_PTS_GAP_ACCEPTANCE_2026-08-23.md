# M6 fabric PTS-gap event acceptance

Status: **accepted as event evidence** on 2026-08-23 for the retained
`base_only_bilinear / tos_daylight / fine-fabric-texture` row.

The authoritative event is the captured PTS-gap event at event/frame index 4,
not a histogram scene-cut. Its recorded PTS is `667000 us` with
`ptsDeltaMs=542`, against `expectedFrameIntervalMs=21.3900261` and the
unchanged `ptsGapMultiplierGreaterThan=2.5` rule. The PTS-gap trigger is
therefore `542 > 21.3900261 * 2.5`. The recorded histogram delta remains
`0.324228227`, below `histogramDeltaGreaterThan=0.65`, and is not used as the
cause.

The event trace retains the raw runtime labels (`detectorSceneCut=true`,
`resetCause=detector_scene_cut`, and `ghostCause=detector_scene_cut`). The
assembly layer adds a separate cause classification, `pts_gap`, together with
the event PTS, detector inputs, and versioned threshold provenance. It does not
relabel the runtime detector event or lower any threshold.

The retained baseline metric row is complete for the event contract:
`static_flicker=0.005329864`, `edge_variance=0.000009484`,
`motion_compensated_error=0.012313397`, `ghost_duration_frames=1`, and
`reset_recovery_frames=0`. The zero reset value is a measured value, not a
blank or fabricated unavailable value.
