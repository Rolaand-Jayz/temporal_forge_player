# Reviewer B — Blind Cross-Review of CANDIDATE A

Date: 2026-09-05 · Reviewer: GLM 5.3 Flash (Review B) · Baseline: `0425ab2e5a23d36c0de4cfc7b395a7ea7f83c148` · Candidate commit: `db19cfb34`

All captures and numbers below were produced by Reviewer B from independently built binaries of both trees through the real Temporal Forge temporal pipeline (`run_temporal_quality.sh`), with per-cell provenance (binary/input/reference hashes, git commit, worktree diff hash, player environment) retained in each capture's `artifacts/`.

## Candidate under review

CANDIDATE A is a minimal, focused submission: **one production line** in `MotionEstimator::aggregateConfidence` (`src/motion/MotionEstimator.cpp`) — when a nonempty motion field contains no valid in-frame coverage (`covered <= 0`), it now returns the caller-configured `emptyConfidence` instead of a hard-coded `0.25` — plus one regression test (`tests/motion_estimator_tests.cpp`) and one evidence document (`docs/active/MOTION_CONFIDENCE_FALLBACK_20260905.md`).

Behavioral context established during review: the affected scalar is the frame-level `historyConfidence` consumed by (a) the prepass history-confidence gate (default threshold 0.55), (b) the scene-cut detector inputs, and (c) the learned composition blend (`slot1[3] = learnedStrength × effectiveConfidence`). Under the default configuration the prepass gate does not flip (both 0.25 and the 0.5 fallback are below 0.55), but the learned composition share **doubles** on affected frames.

## Independent reproduction results

| Claim | Result | Classification |
|---|---|---|
| Fallback honors configured policy instead of hard-coded 0.25 | Function-level driver linked against both trees: baseline `0.250000`, candidate `0.800000` with fallback 0.8. Unit test passes on the candidate; full CTest 20/20 reproduced. | **SURVIVES** |
| Four-tier metrics table (240p/360p/720p/1080p) | Reproduced within window-length expectations: 240p 0.762449 vs author 0.762697; 360p 0.825059 vs 0.825759; 720p 0.943237 vs 0.946578; 1080p 0.913008 vs 0.916524 (author used 8 frames + 2 warmup; this review used 16 + 8). | **SURVIVES WITH QUALIFICATION** (author-only numbers; no baseline pairing in their evidence) |
| No regression on the normal temporal path | Paired baseline-binary vs candidate-binary runs, identical configuration: outputs **byte-identical** on tos_daylight at all four tiers and on sintel_rooftop 720p/240p; both binaries individually self-deterministic. The only reproducible pixel difference in the entire matrix is 3 startup frames on sintel_cave 1080p, with mean SSIM identical to 6 decimals, min SSIM differing 1e-6, and temporal delta error identical. | **SURVIVES WITH QUALIFICATION** (proven by this review's pairing, not by the candidate's evidence, which contained no baseline-binary runs) |
| The fixed path is reachable in the real pipeline | Dispatch-trace telemetry found candidate-vs-baseline confidence divergence on sintel_rooftop 720p/240p and sintel_cave 1080p — roughly 1 frame per 16–24-frame window (~1%); tos_daylight and tos_debris never trigger it. A static (single-frame) stream also triggers it: 2 of 12 frames pixel-differ at sub-metric level (SSIM delta −4e-6). | **SURVIVES WITH QUALIFICATION** (trace telemetry itself has run-to-run noise in both binaries; pixel ground truth is authoritative) |
| "Rejected alternatives were tested through the real pipeline" | No retained artifacts exist for the claimed learned-strength / best-findings / photometric-gate / current-weight / validation-threshold arms beyond six 426×240 cells that differ by ≤0.001 SSIM and carry no distinguishing environment records. | **UNSUPPORTED** (possibly true, not verifiable from the submission) |
| Lattice safety | Calibrated `periodic_lattice_detector.py` on the 3 reproducibly-differing sintel_cave 1080p frames: candidate-vs-baseline residual lattice energy 2.4e-13 / 7.2e-14 / 9.0e-12 (aperiodic micro-noise); candidate-vs-Lanczos-reference control 1.99–2.19e-07, baseline-vs-Lanczos 1.99–2.19e-07 — both below the calibrated 5.0e-07 tripwire, and the two arms are statistically indistinguishable. | **SURVIVES** — no candidate-introduced periodic contamination |

## Adversarial checks (both binaries, paired)

- **Scene cut** (daylight|cave hard cut, 720p, 12 scored frames): candidate and baseline **byte-identical across all frames**; identical metrics (SSIM 0.940790 / min 0.933060 / derr 0.250). No cut-specific divergence, no reset blowup.
- **Static content** (single frozen frame, 720p): SSIM 0.770999 vs 0.771003 (−4e-6); frame-to-frame output delta 0.0031 vs 0.0026 — marginally less stable on 2 of 12 frames, within noise; no smear.
- **Fast motion** (tos_debris): covered by the paired matrix — no regression at any tier.
- **Performance**: paired 32-frame probe, median ratio 1.012 (noise). Zero cost by construction (one scalar branch).

## Contracts

No motion direction/units/scaling changes, no jitter-semantics or history-publication changes, no commit/rollback or reset changes, no geometry-ordering changes, no lattice-sensitive path touched. The change only alters a reported scalar in a rare branch and makes it honor operator configuration — a policy-consistency improvement.

## Evidence weaknesses found in the submission

1. The four-tier table was produced from the candidate binary only — no baseline-binary pairing, so "no regression" was unproven by the submission itself.
2. The documented binary hash reproduces only in the author's build directory.
3. The capture set never demonstrated the fixed path firing; the occurrence rate (~1% of frames, sintel scenes and static streams only) was established by this review.
4. Capture windows were minimal (8 frames, 2 warmup).
5. The "rejected alternatives" claim has no retained evidence.

## Verdict

- **Recommendation: ACCEPT.**
- The change is correct, minimal, honest (it explicitly claims no spatial-quality win), regression-free across the full four-resolution matrix, lattice-clean, and free.
- Its evidence pack is weaker than its code: the substantive no-regression claim only became provable through independent baseline pairing, and the most interesting fact discovered in review — that the fixed path occurs naturally on sintel scenes and static streams at ~1% of frames — was unknown to the submission.
- Weaknesses are evidential, not behavioral; nothing blocks merge.
