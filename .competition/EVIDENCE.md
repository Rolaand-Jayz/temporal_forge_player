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

## E5 — accumulation dose-response (rectification scale)

- CONFIGURATION: tos_daylight 720p integrated arm, rectification 1.0 / 0.5 /
  0.25 / 0.1 (lower current weight = stronger accumulation).
- MEASUREMENT: derr 0.905 -> 0.873 -> 0.873 -> 0.878; SSIM 0.8689 -> 0.8678
  -> 0.8665 -> 0.8655. Saturating, slightly negative on SSIM.
- CONCLUSION: the prepass accumulation barely reaches the final output (the
  learned per-pixel history blend is small); the flicker lives in the neural
  reconstruction itself. Static-gated accumulation in the prepass would not
  pay. Abandoned.

## E6 — composition sweep + definitive motion falsification

- CONFIGURATION: tos_daylight 720p; learnedStrength override 0.35 / 0.75;
  CAS 0.35 / 0.5; motion scale x3; plus 240p CAS 0.35 and rooftop CAS 0.35.
- MEASUREMENT:
  - ls0.35:  SSIM 0.898530, min 0.882764, derr 0.446  (baseline 0.868871 /
    0.905; Lanczos 0.950882 / 0.495)
  - ls0.75:  SSIM 0.813887, derr 2.005  (catastrophic)
  - cas0.35: 0.851570 / 1.367; cas0.5: 0.846893 / 1.587 (CAS past optimum,
    harmful at 0.20 default already; 240p cas035 SSIM worse, rooftop worse)
  - motion scale x3: 0.854462 / 1.160 vs codec 0.855945 / 1.123 — output
    nearly unchanged under 3x vector exaggeration.
- CONCLUSION: (1) the composition blend between the classical catmull-rom
  base and the neural reconstruction is the dominant quality lever; 0.35
  beats both the 0.55 baseline and Lanczos on temporal error while closing
  a third of the SSIM gap. (2) The motion-field lever is definitively
  closed: direction inversion (E1), field stabilization (E3), timescale
  (E4), and 3x exaggeration (E6) all move final metrics by <= ~0.002.
- MECHANISM: learnedStrength = mix(classical base, neural reconstruction)
  in the postpass. An enabled Quality Lab config flattens the legacy
  resolution-adaptive heuristic (0.05-0.55 by source height) to a constant.
- NEXT ACTION: per-tier dose-response sweep (E7), then implement a
  tier-adaptive strength as the candidate.

## E9 — final candidate vs baseline (frozen criteria shape: 4 tiers x 4 scenes)

- CANDIDATE STATE: commit `c1d1154eb` ("Add tier-adaptive learned
  composition strength"); binary `build-fast/temporal_forge_player`
  sha256 `ccb64de00a15ec658eb84c4ac81dff2392dde237f1d4e36b3e1f1a060bad3b89`.
  Candidate config: `adaptiveLearnedStrength: true` in the swarm config
  (table: sourceHeight <=540 -> 0.35, <=760 -> 0.20, else 0.18).
- BASELINE ARM: identical environment, config WITHOUT the adaptive flag
  (flat 0.55, the frozen default). Single-variable pairing.
- MEASUREMENT (mean SSIM / temporal delta abs error, 16-frame paired):
  - MEAN dSSIM +0.0545, MEAN dDERR -0.3854 vs baseline.
  - SSIM wins 14/16 (losses are sintel_cave 240p/360p, both -0.001, i.e.
    neutral); derr wins 9/16; the 7 derr losses are confined to 240p/360p
    (max +0.841 on tos_daylight 240p) where the candidate still beats the
    Lanczos control on the same cell (1.250 vs 1.433).
  - Largest wins: sintel_rooftop 1080p +0.116 SSIM / -0.690 derr;
    sintel_rooftop 720p +0.087 / -0.743; tos_daylight 1080p +0.098 /
    -0.900; tos_debris 720p +0.071 / -1.198.
  - vs Lanczos control: candidate SSIM gap closed from -0.081..-0.033 to
    -0.043..-0.004; candidate derr beats Lanczos on 10/16 cells (baseline
    beat it on 5/16).
- CONCLUSION: the tier-adaptive composition is the candidate. It delivers
  the largest measured overall improvement available in this pipeline.

## E10 — self-adversarial validation

- SCENE CUT (daylight|cave hard cut, 720p, 12 scored frames): candidate
  0.924808 mean SSIM / 0.915044 min / derr 0.064 vs baseline
  0.851059 / 0.818740 / 1.342. Per-frame traces show the same cut-dip
  structure with the candidate uniformly higher; no reset blowup.
- STATIC CONTENT (single frozen frame, 720p): candidate SSIM -0.0019 vs
  baseline (noise level); both outputs frame-to-frame delta <= 0.016 —
  no smear or denoise-loss regression from the weaker neural share.
- FAST MOTION: tos_debris (fast debris) wins at every tier (720p:
  +0.071 SSIM / -1.198 derr vs baseline).

## E11 — performance

- Paired 32-frame captures, identical environment, 3 repetitions each:
  candidate median 8.53 s vs baseline 8.63 s wall (ratio 0.988, within
  noise). The candidate changes one composition uniform; compute cost is
  identical by construction. Measured cost regression: none.

## Lattice check

- Calibrated periodic-lattice detector
  (`lattice_corruption_diagnostic/periodic_lattice_detector.py`), 64
  matched pairs (every 4th frame of all 16 candidate cells) against the
  aligned Lanczos-scaled reference frames as controls: candidate residual
  lattice energy max 2.819e-3 / median 3.968e-5; the flat-0.55 baseline
  on the same cells max 2.738e-3 / median 4.194e-5. The candidate's
  lattice profile is statistically indistinguishable from the frozen
  baseline's; no candidate-introduced periodic contamination.

## Final candidate summary

- Commits: `04658d54e` (opt-in global anchor; default-off, null result
  retained honestly), `629ab1d6e` (motion timescale normalization;
  default-off correctness improvement), `c1d1154eb` (tier-adaptive
  composition; the quality candidate, default-on via swarm config).
- Candidate binary: ccb64de0... (built from `c1d1154eb`).
- HONEST WEAKNESSES:
  1. SSIM remains below the Lanczos spatial control at every tier
     (closed from ~-0.08 to -0.004..-0.043 depending on tier/scene).
  2. 240p/360p temporal delta regressed vs the 0.55 baseline on some
     scenes (up to +0.84 on daylight 240p) — the price of the +0.03-0.05
     SSIM gain there; candidate derr still at or below the Lanczos
     control on those cells except cave 240p (+0.062).
  3. sintel_cave is strength-insensitive below 720p (neutral outcome).
  4. The temporal-delta metric ignores PTS and is a stability proxy,
     not a motion-fidelity metric.
  5. The motion-field mechanisms I built (anchor, timescale) are
     absorbed by the pipeline; they are retained as opt-in tooling, not
     quality claims.
- The dominant remaining limiter is the neural reconstruction itself
  (softness + flicker at near-1:1 tiers); improving it requires weight
  or feature work outside this competition's safe scope.
