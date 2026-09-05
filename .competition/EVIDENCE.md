# Competition R&D evidence log (GLM 5.3 Flash — motion-campaign-devil)

Baseline: frozen commit `0425ab2e5` (branch `motion-campaign-devil`).
Baseline binary: `build-fast/temporal_forge_player` sha256 `4bd59c5fde526703…`
built clean from `0425ab2e5`. All captures: 16 scored frames after 8 warmup,
software decode (`TFORGE_DISABLE_HW_DECODE=1`), zero jitter, color history +
recurrent on, forced 1920x1080 output, config `agent_recheck_current`
(learned strength 0.55, catmull-rom base, sharpen none), CAS 0.20 (engine
default; see E2). Every capture's `artifacts/` retains binary/input/reference
hashes, git commit, worktree diff hash, and player log.

Common environment for all arms: same as above unless stated.

## E1 — motion sensitivity (is the temporal pipeline sensitive to motion at all?)

- HYPOTHESIS: the R&D-round null results (motion arms ≈ zero motion within
  3e-5 SSIM over 8-frame windows) understate motion's value because history
  effects accumulate over longer sequences.
- CONFIGURATION: tos_daylight 1280x720 -> 1920x1080; arms zero / codec /
  refined / codec+inverted-sign.
- MEASUREMENT (mean SSIM vs lossless Lanczos control; temporal delta abs
  error vs reference):
  - zero   0.701655 / derr 0.4328
  - codec  0.855945 / derr 1.1231
  - refined 0.855149 / derr 1.1438
  - invert 0.855562 / derr 1.1214
  - Lanczos 0.950882 / derr 0.4950
- OBSERVATION: over 16 frames, motion-bearing arms beat the zero arm by
  +0.154 SSIM. Vector *direction inversion* has no effect (<= 0.0004 SSIM).
- ALTERNATIVE EXPLANATION: the zero-arm penalty is dominated by the BLACK
  history channel (no seeds -> `historyModel = 0`), not by misalignment; the
  prepass photometric gate neutralizes wrongly-aligned but real history,
  which is why inversion is free.
- CONCLUSION: (1) the short 8-frame R&D windows hid the true value of the
  history channel; (2) within motion-bearing arms, vector quality/direction
  is nearly irrelevant to final SSIM — the pipeline self-protects.
- NEXT ACTION: find why motion arms still trail Lanczos by 0.095 SSIM and
  2.3x delta error; locate the flicker source.

## E2 — configuration ceiling

- HYPOTHESIS: the R&D "integrated profile" gain (refined + edge-aware +
  fallback) is reproducible on the frozen baseline.
- CONFIGURATION: tos_daylight; tiers 240p/360p/720p/1080p; arms codec,
  integrated (`TFORGE_FSR4_INTEGRATED_TEMPORAL=1`), +`TFORGE_FSR4_CAS_STRENGTH=0.20`.
- OBSERVATION: `TFORGE_FSR4_CAS_STRENGTH` is a no-op because CAS 0.20 is
  ALREADY the engine default (`cas_only` == codec exactly). Integrated:
  720p SSIM 0.8689 (+0.0130 vs codec), derr 0.905 (vs 1.123). 240p: SSIM
  +0.0044, derr worse (0.519 vs 0.408).
- Tier map (integrated): FSR vs Lanczos SSIM -0.029 (240p), -0.045 (360p),
  -0.082 (720p), -0.081 (1080p); derr wins at 240p (0.52 vs 1.43) and 360p
  (0.05 vs 1.07), loses at 720p (0.91 vs 0.50) and 1080p (0.72 vs 0.13).
- CONCLUSION: the temporal pipeline's real headroom is at 720p/1080p, where
  it loses on both axes; at 240p/360p it already crushes Lanczos temporally.

## E3 — global-anchor stabilization (first candidate)

- IMPLEMENTATION: commit `04658d54e`. Median-seed anchor refined to
  quarter-pel over a 64-patch spread set; agreeing seeds replaced by the
  anchor; deviating seeds kept; empty-seed fallback upgraded to the same
  multi-patch search. Unit-tested.
- MEASUREMENT: output metrics unchanged (720p integrated+anchor 0.868510 /
  derr 0.9024 vs 0.868871 / 0.9051).
- FALSIFICATION (mechanism check): motion dumps show the anchor DID engage
  and reduced field jitter on several frames, but the field's MEAN motion
  itself swings 4x between adjacent frames (1.30 -> 0.37 -> 0.78 px), and
  the anchor inherits that instability (its coarse radius is only +-1 px).
- ROOT CAUSE: the benchmark clips are ~70% B-frames (ffprobe: tos_daylight
  720p = 175 B / 60 P / 4 I). In I B B P display order, B back-vectors span
  1 display frame but P vectors span 3; the seed field alternates between
  two incompatible timescales.
- CONCLUSION: anchor-over-a-poisoned-field is a null result; fix the
  timescale first.
- NEXT ACTION: E4.

## E4 — timescale normalization

- IMPLEMENTATION: commit `629ab1d6e`. Decoder annotates each frame with the
  display-frame distance to its past reference (B=1, P=group distance);
  behind `TFORGE_FSR4_MOTION_TIMESCALE_NORMALIZE` the decode loop rescales
  group vectors to per-display-frame displacement before validation.
  Contract preserved: current->previous-displayed-frame, source pixels.
- MEASUREMENT (tos_daylight 720p): codec+TSN 0.855249 / derr 1.1383 vs
  codec 0.855945 / 1.1231; integrated+TSN 0.869096 / 0.8965 vs integrated
  0.868871 / 0.9051; integrated+TSN+anchor 0.868628 / 0.8963.
- FIELD CHECK: P-frame magnitudes correctly drop to the B-frame scale
  (frames 1/5: 1.30->0.38, 1.56->0.48), but the remaining frame-to-frame
  variation is genuine content variation (walking person), not poison.
- CONCLUSION: timescale normalization is correct-by-construction but a
  null result for output quality — the third independent demonstration
  that the pipeline absorbs motion-field changes in the sub-2px regime.
  Retained as a correctness improvement (cheap, contract-truer), not a
  quality candidate.
- PIVOT: flicker decomposition (E2 captures, frame-to-frame RGB delta on
  tos_daylight 720p integrated) shows STATIC regions flicker as much as
  moving regions (4.4 vs 4.1, 5.1 vs 4.6 mean 8-bit delta). The temporal
  deficit is a global reconstruction instability, not a motion failure.
  Next lever: strength of the temporal accumulation itself
  (`TFORGE_FSR4_HISTORY_RECTIFICATION_SCALE` dose-response), then a
  per-pixel static-gated accumulation if the dose-response is positive.
