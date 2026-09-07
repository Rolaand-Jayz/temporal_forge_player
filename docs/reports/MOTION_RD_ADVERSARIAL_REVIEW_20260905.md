# Motion-input R&D round — Devil's Advocate adversarial review

**Reviewer:** Devil's Advocate branch `motion-campaign-devil`
**Date:** 2026-09-05
**Reviewed round:** Luna's completed motion-input R&D
(`quality-lab-vibecoder` @ `0425ab2e5`; review baseline `92fab588c`)
**Binary under review:** built in this worktree from `0425ab2e5`
(`build-devil/temporal_forge_player`, ctest 20/20 runnable pass).
**Rig validation:** this binary + Luna's published env/protocol reproduced her
recorded values exactly on two tiers (720p rooftop zero:
SSIM 0.885053 / tdae 0.211071; 1080p rooftop zero: SSIM 0.961679 vs her
0.961677). Divergent runs below are therefore attributable to the tested
variables, not to rig drift.

## Method

Targeted falsification through the real production temporal path
(`run_temporal_quality.sh` + this branch's binary; native INT8 graph verified
per run via dispatch trace `native-int8 graph complete` / `s0z` bit 12).
Arms differ only in the env selectors under test. Evidence trees:
`.devil_batch1` (generic-path, superseded), `.devil_batch2` (720p rooftop
falsification matrix), `.devil_batch3` (cave/240p/1080p/360p coverage +
single-history isolation), `.devil_batch3b` (codec-arm blend triangle),
`.devil_batch4` (360p), plus read-only re-aggregation of Luna's
`.campaign_motion_multiframe/` and `.campaign_gate_trace/` artifacts.

## Resolution coverage

Four source tiers (426×240, 640×360, 1280×720, 1920×1080) → 1920×1080,
sintel_rooftop unless noted, 8 frames + 8 warmup, Luna's exact frame protocol.

| tier | zero | codec | gate always-on | gate always-off | offline dense | best-findings OFF | Lanczos |
|---|---:|---:|---:|---:|---:|---:|---:|
| 240p | 0.667170 | 0.667197 | 0.667159 | 0.667181 | 0.667369 | **0.667907** | 0.657670 |
| 360p | 0.745986 | 0.745904 | — | — | — | **0.746846** | 0.741193 |
| 720p | 0.885053 | 0.884663 | 0.884593 | 0.885315 | 0.885068 | **0.885965** | 0.888096 |
| 1080p | 0.961679 | 0.961416 | 0.961415 | 0.961819 | 0.961644 | **0.961963** | 0.965048 |

sintel_cave 720p: zero 0.879162, offline 0.879406, gate-on 0.879284,
profile-off 0.879182 — replicates Luna's one hidden reversal (offline > zero
by +0.000244 on my rig vs her +0.001256) at noise-adjacent magnitude.

## Findings

### F1 — "Better correspondence provides no repeatable headroom" (C2)

**CLAIM UNDER REVIEW:** Luna's central conclusion that zero motion is best or
near-best across the matrix and that improved correspondence (offline dense
flow) provides no repeatable gain, therefore no heavier estimator or
confidence change is justified.

**OBSERVATION:** The sign of the result reproduces (zero ≥ codec/refined at
default threshold at every tier; motion arms never win by >0.0005 SSIM).
But the causal basis does not survive: the temporal consumer through which
motion would express itself is effectively closed.

**CODE PATH:** `shaders/fsr4/prepass_pq_eotf.comp:398-483` (frame gate +
per-pixel coverage + history publication), `shaders/fsr4/postpass_composite.comp:536-550`
(`temporalModelColor = mix(upscaledColor, reprojectedColor, historyBlend)`,
`historyBlend = sigmoid(decoder-c3)`), `src/render/Fsr4DispatchHarness.cpp:3634-3640`
(`currentWeight` default 0.02 under best-findings).

**EXPERIMENT:** (a) force gate always-on/off (threshold 0.0001/0.99); (b)
`TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT` 0.0/1.0; (c)
`TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND=1` (bit 1024: bypass the
history blend entirely); (d) `TFORGE_FSR4_DISABLE_BEST_FINDINGS=1`; (e)
zero/codec/refined/offline arms; all cross-tier.

**CONTROL:** rig reproduction of Luna's exact values (above); dispatch-trace
verification that each flip changed the intended runtime state (gate pass
counts 16/16 vs 0/16, reprojection dumps identical/differing as expected).

**CONFIGURATION:** campaign env parity verified via recorded
`player_environment.txt` diff (identical except paths/experiment IDs).

**RESOLUTION(S):** 240p, 360p, 720p, 1080p (all four), plus cave scene at 720p.

**EVIDENCE:** Bypassing the history blend entirely (singlehist) shifts SSIM by
+0.000026 (zero arm) / +0.000032 (codec arm) — at the run-to-run noise floor
(~0.00002, measured via duplicate runs). Yet the reprojected history those
runs discarded differs by 0.106 mean FP16 RGB (zero vs codec dumps). Pixel
consequence of everything temporal: RMSE ≈ 0.35/255 with isolated 8-bit
peaks. `currentWeight` 0.02→0.0 shifts SSIM by 0.000007.

**COMPETING EXPLANATION:** "Codec motion never reached the consumer" —
rejected: dense fields differ (codec vs zero: 100% of pixels; magnitudes up
to 141 px), frame confidence differs (0.14–0.70 codec vs 0.38–0.56 zero),
gate pass counts differ (10/16 vs 1/16), reprojection content differs
(0.106 mean diff). The consumer receives different history and blends it at
~zero weight.

**FALSIFICATION ATTEMPT:** tried to make motion matter: gate pinned open
(threshold 0.0001), confidence forced via empty-motion pinning, offline
Farneback dense flow injected through the canonical sidecar path. Best
motion-arm result still within 0.0005 SSIM of zero-motion, and
best-findings-OFF (no history/recurrent/gate/currentWeight at all) beat
every temporal configuration at every tier.

**RESULT:** Luna's statement is true of this pipeline but vacuous as a
statement about motion quality: the pipeline multiplies reprojected history
by `sigmoid(decoder-c3) ≈ 0` and composes ~72–78% spatial base
(`learnedBlend = learnedStrength × effectiveConfidence ≈ 0.23–0.28` is the
learned fraction). No motion-quality intervention can express itself.

**CONFIDENCE:** high (exact rig reproduction, trace-verified flips, four
tiers, pixel-level confirmation).

**WHY IT MATTERS:** the campaign's "no headroom" conclusion cannot justify
estimator decisions — the estimator is upstream of a closed gate. The real
bottleneck is downstream motion consumption (history-blend weight +
composition weighting), which Luna's campaign did not test.

**R&D CONCLUSION:** survives with qualification (the qualification negates
the stated mechanism).

**NEXT HIGH-INFORMATION EXPERIMENT:** open the consumer deliberately
(diagnostic violation): set decoder-c3 history-blend logit to a controlled
positive bias or override `historyBlend` to a constant, re-run zero vs
offline-dense, and measure whether better motion pays once history is
actually blended. This directly measures the headroom the closed consumer
hides.

### F2 — Confidence gate "behaves as configured"; gate not dominant limiter (C4)

Per-frame gate behavior reproduces exactly as Luna's derived analysis
suggested (my traces: zero arm fails 15-16/16 at default; codec 6-16 pass
depending on arm; threshold sweep shifts pass counts as configured). Her
claim that the gate is "not demonstrated as the dominant limiter" survives,
but for a stronger reason than she stated: whether the gate passes or fails
changes output SSIM by ≤0.0007 because gate failure publishes black history
that the closed postpass blend would ignore anyway.
**R&D CONCLUSION:** survives with qualification.

### F3 — 1080p tier was passthrough; now a real motion path (C1)

Runtime-verified: with her `modelW == decodedW && modelH == decodedH`
passthrough guard, the 1080p tier dispatches the native 14-pass INT8 graph on
a 1280×720 model grid (`decoded 1920x1080 -> model 1280x720 -> 1920x1080`),
uploads motion, and produces arm-dependent output (codec vs zero differ
beyond noise at 1080p). The pre-fix bug claim was not independently
re-executed (revert build skipped as low-information), but the committed
contract test plus the current behavior are consistent.
**R&D CONCLUSION:** survives.

### F4 — Dominant resolution effect is source-detail loss (C3)

Absolute SSIM falls sharply at low source tiers on my rig too. Additional
resolution-dependent structure Luna did not report: FSR beats Lanczos at
240p (+0.0095) and 360p (+0.0048) but loses at 720p (−0.0030) and 1080p
(−0.0031) — the spatial value of the pipeline flips sign with tier, and the
best-findings temporal profile is a small consistent negative at all tiers.
**R&D CONCLUSION:** survives with qualification.

### F5 — Offline dense flow "is not an upper-bound win"

**Unsupported.** The offline arm cannot measure dense-flow headroom in this
campaign: (i) its replay confidence is pinned at ≈0.5 → the frame gate
rejected history on 16/16 frames in my run (her default-confidence keys
included), so dense flow was not consumed even as history; (ii) the closed
postpass blend would have muted it anyway. The one place it beat zero
(720p cave, +0.001244 her / +0.000244 mine) came through conditioning-only
effects at noise-adjacent magnitude. **R&D CONCLUSION:** unsupported (the
arm never tested the hypothesis).

### F6 — New finding: the promoted best-findings profile is a net negative

At every tier, `TFORGE_FSR4_DISABLE_BEST_FINDINGS=1` (no history/recurrent/
gate/currentWeight/photometric/confidence-map profile) produced the best
SSIM of all arms (240p +0.0007, 360p +0.0009, 720p +0.0009, 1080p +0.0003
over the promoted profile's zero arm). Luna's matrix contained no such
control. The campaign's six arms all sit inside a profile that is never
better than its own absence.
**R&D CONCLUSION:** contradicts the premise that the promoted profile is
worth keeping enabled as default for quality (visual review reads the
profile-off arm as subtly softer at 1080p despite its SSIM edge, so the
flip is a metric-level, not perceptually proven, win — but never a loss).

### F7 — Refinement does not change output despite changing the field

Codec-refined alters 0.75–5.4% of dense vectors (up to 36 px) versus raw
codec, yet output differs by ~0.00006 SSIM. Consistent with Luna's numbers
(refined ≈ codec ≈ autocheap). **R&D CONCLUSION:** survives (same
qualification as F1: nothing moves because the consumer is closed).

### F8 — Evidence infrastructure: environment-dependent backend trap

The native INT8 path silently degrades to the generic split-graph when
`resources/fsr4/native_i8/**.spv` (gitignored, untracked) is absent, changing
rooftop-720p zero-arm SSIM from 0.885 to 0.696. Any capture tree without the
vendored shaders measures a different pipeline while logging a normal-looking
run. My first falsification batch fell into exactly this trap.
**R&D CONCLUSION:** observation; a provenance hazard for all future rounds
(recommend: fail-closed warning or tracked manifests of required assets).

### F9 — Uncommitted deletion of the lattice-qualification record in Luna's worktree

**OBSERVATION (2026-09-06):** `quality-lab-vibecoder`'s committed tip
(`0425ab2e5`) is unchanged and retains the full lattice saga in
`docs/LATTICE_CORRUPTION_DIAGNOSTIC.md`, but her *working tree* carries an
uncommitted edit deleting 132 lines: the failed full-temporal-state
qualification (2026-09-03; "clear lattice" under geometry mismatch with
history+recurrent enabled — the configuration the motion campaign and this
review ran under), the FP16-resolve candidate, the production-semantic PASS
closeout whose scores (`0.023164/0.016783/0.030382`) are exactly the ones
committed `MOTION_CAMPAIGN.md` cites for lattice safety, and the 2026-09-04
human-review reopen that root-caused the GPU bicubic downsampling prefilter
and fixed `bicubic_prefilter.comp`. The replacement text downgrades the
record to "fresh qualification ... remains required before campaign
approval."

**COMPETING EXPLANATION:** legitimate pre-campaign restructuring (the docs
system allows rewriting the active record before a fresh qualification
round) versus evidence scrubbing. The committed history is intact and the
deleted evidence trees still exist on disk
(`lattice_p0_recurrent_qualification_20260903/`,
`lattice_corruption_diagnostic/`), so nothing is lost yet.

**WHY IT MATTERS:** (i) the motion campaign's lattice-safety cross-check
cites a PASS section that her working tree currently deletes — the
cross-check is dangling against her live tree; (ii) the retained fix
(`bicubic_prefilter.comp` linear downsample resolve) is part of the baseline
both competitors would build on, and its entire justification now lives only
in committed history that her local edits remove; (iii) my independent
visual pass flagged faint dot patterns (needs zoomed confirmation) in the
same history+recurrent-enabled configuration that once failed with "clear
lattice".

**R&D CONCLUSION:** observation, no verdict on intent; but the frozen
baseline for the next round MUST be taken from commits (`92fab588c`), never
from either worktree, and the lattice doc must be reconciled before the
competing round starts.

## Verification pass (2026-09-06)

All six decisive runs re-executed on a fresh day against the same binary;
values reproduce within run-to-run noise (worst |Δ| 0.000043, typical
≤0.000003):

| run | verify | round-1 | Luna reference |
|---|---:|---:|---:|
| zero_def | 0.885052 | 0.885053 | 0.885053 |
| codec_def | 0.884663 | 0.884663 | — |
| codec_t000 | 0.884596 | 0.884593 | — |
| codec_t099 | 0.885314 | 0.885315 | — |
| singlehist | 0.885036 | 0.885079 | — |
| spatial | 0.885966 | 0.885965 | — |

Native path and gate states re-verified via dispatch trace. The findings
(F1–F7) are deterministic properties of the binary+configuration, not
one-shot captures. Evidence: `.devil_verify/`.

## Adjudication

1. **Survived:** C1 (1080p real-FSR fix), C4 (gate behaves as configured),
   C5 (lattice tripwire values reproduce; no promotion of protected-contract
   changes — her diff is trace-only), F7-style refinement no-op.
2. **Survived with qualification:** C2 (no-headroom — true only because the
   consumer is closed), C3 (resolution effect — plus an unreported
   FSR-vs-Lanczos crossover), C4 (gate not dominant — for a deeper reason).
3. **Unsupported/contradicted:** the offline-dense upper-bound claim (F5);
   implicitly, any estimator-ranking interpretation of the matrix (F1).
4. **Unresolved:** whether opening the consumer (historyBlend bias /
   composition weight) lets better motion pay; whether decoder-c3 ≈ 0 is
   trained behavior (weights are frozen by policy) or an integration defect;
   perceptual (not metric) consequences of the profile flip (F6) — visual
   review this round found no ghosting/lattice regression attributable to
   any arm (see review notes below), but a longer-sequence perceptual test
   is outstanding.
5. **Safe to retain from her round:** the 1080p passthrough fix; the
   dispatch-trace `historyGateEnabled/historyGatePass/threshold` fields; the
   motion campaign runner and manifest discipline; all evidence trees.
6. **Should NOT enter the baseline:** any conclusion that "heavier estimators
   are not justified" used to close the motion workstream (the test was
   structurally unable to detect estimator value); the offline-dense arm in
   its current gated configuration as an "upper bound".
7. **Strongest bottleneck evidence by resolution:** identical mechanism at
   all four tiers — motion is produced, delivered, and reprojected
   (conditioning reaches the network), but the final history blend weight
   (sigmoid(decoder-c3)) ≈ 0 and the learned fraction of composition is
   ~0.23–0.28, so output-influence of the entire temporal machinery is
   ≤0.002 SSIM. At 240p/360p the pipeline's value over Lanczos is spatial,
   not temporal.
8. **Demonstrated headroom from better motion, as wired today:** ~0
   (bounded by flips at ≤0.0005 SSIM; noise floor ~0.00002). Potential
   headroom if the consumer is opened: unmeasured — the single highest-value
   experiment for the next round (F1 next-step).
9. **Strongest justified directions for the improvement round:** (a) open and
   instrument the consumer — measure quality vs forced historyBlend values
   with matched motion arms; (b) investigate why decoder-c3 saturates closed
   (input feature lanes 1/3-6 conditioning) without touching frozen weights —
   e.g., feature normalization or the history-input contract; (c) re-evaluate
   the best-findings profile default given F6; (d) fix the offline-dense arm
   (gate-neutral confidence) before ever citing it as an upper bound again;
   (e) make native-shader asset absence fail loudly (F8).
10. **Clean baseline for both competitors:** `92fab588c`
    (current `motion-campaign-devil` reset point; common ancestor of both
    branches). Luna's tip `0425ab2e5` = baseline + her 12 R&D commits; my
    review additions live only on `motion-campaign-devil` after `0425ab2e5`.
    The baseline must be taken from commits only — Luna's worktree currently
    carries uncommitted lattice-record deletions (F9).

## Visual review

Delegated to the vision worker over 14 frames (`.devil_review/`, 1080p
zero/codec/spatial, 240p zero/spatial, cave offline/zero, frames 0003/0007).
Results (INFERENCE-grade; single-frame remote vision passes, no
interleaved playback):

- No clear periodic/lattice contamination in any arm; only faint, frame-
  inconsistent "dot pattern" flags (codec arm flat areas at 1080p,
  spatial_0007, both 240p arms) that need zoomed-crop confirmation before
  any claim. Luna's C5 lattice-safety conclusion is not contradicted.
- Ghosting: the codec arm shows visible ghosting around moving figures at
  1080p (center figure, stall figures, flying figure) that the zero arm
  lacks; the zero arm shows only trace ghosting on the sky figure in frame
  0007. Cave: zero_0007 shows slight silhouette ghosting absent in the
  offline arm. Consistent with motion conditioning slightly degrading
  moving-content rendering.
- No blocking or ringing visible in any arm.
- Sharpness at 1080p: vision reads zero as crisper than both codec and
  spatial (subtle). Note the tension with SSIM: spatial wins SSIM at every
  tier while reading slightly softer visually — SSIM is not tracking the
  perceptual preference here, which further cautions against metric-only
  promotion decisions in the next round.
