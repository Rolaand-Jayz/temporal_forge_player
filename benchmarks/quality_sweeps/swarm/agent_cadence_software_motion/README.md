# Corrected-cadence software-motion comparison

This is a diagnostic campaign record, not a runtime-default change.

All arms used the same four real `426x240` high-quality clips, 36 warmup
frames, 24 scored frames, and a `1920x1080` output. Captures ran with the
cadence false-scene-cut fix present in the working build. No benchmark image
or source video was modified.

The hardware decode path does not export codec motion vectors on this AMD/
VAAPI setup. The software-motion arms used `TFORGE_DISABLE_HW_DECODE=1` and
`TFORGE_FSR4_ENABLE_COLOR_HISTORY=1` so the FSR path received the vectors
available from the same encoded sources.

## Result

`learned=0.15` was the best trade among the tested strengths: it improved
SSIM over the current default on all four clips and improved temporal error on
daylight and rooftop. Debris and cave still had small temporal regressions.
The `confidenceBlend=0.75` and adaptive variants did not remove those
regressions, so none of these arms is promoted.

The raw metric CSVs remain outside the repository under:

`/tmp/tforge-cadence-metrics-20260824`

`/tmp/tforge-cadence-software-motion-20260824`

`/tmp/tforge-cadence-software-strength-20260824`

`/tmp/tforge-cadence-software-adaptive015-20260824`

`/tmp/tforge-cadence-software-blend075-20260824`

The next experiment should make history trust depend on motion reliability;
raising learned strength or adding global sharpening is not supported by this
comparison.
