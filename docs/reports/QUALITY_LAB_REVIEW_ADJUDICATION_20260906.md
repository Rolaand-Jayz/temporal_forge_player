# Quality Lab review adjudication

**Date:** 2026-09-06  
**Historical baseline:** `0425ab2e5a23d36c0de4cfc7b395a7ea7f83c148`  
**Scope:** PR #1 review findings and production Quality Lab corrections

The historical baseline is immutable provenance. This report distinguishes it
from the corrected production integration; it does not rewrite, delete, or
relabel the historical capture roots.

## Finding dispositions

| Finding | Disposition | Affected category |
|---|---|---|
| Generic neural-target sizing | **CONFIRMED — FIX BEFORE/IN STACK** | Runtime correctness; latent reconstruction quality risk |
| CAS qualification environment variable | **CONFIRMED — PRODUCTION BLOCKER** | Qualification validity; benchmark provenance |
| Missing GPU timing evidence | **CONFIRMED — HISTORICAL EVIDENCE LIMITATION** | Performance evidence only |
| Active-game/contention recording | **CONFIRMED — FIX BEFORE/IN STACK** | Performance provenance only; not image-quality validity |
| Stale campaign-root defaults | **CONFIRMED — FIX BEFORE/IN STACK** | Reproducibility and benchmark provenance |
| Superseded M6 active document | **CONFIRMED — FIX BEFORE/IN STACK** | Documentation authority |
| Audio-only playlist EOF | **CONFIRMED — OUT OF SCOPE** | None in Quality Lab; pre-existing playback behavior |

Copilot's review was not a correctness finding: GitHub reported that PR #1
exceeded its maximum review size. Codex's comment was independently checked
against the historical tree and committed artifacts below.

## Evidence and historical impact

### Generic target sizing

In the historical `computeFsrJitterPair()` implementation,
`nativeInt8FixedTarget()` returns a zero-sized target for geometries without a
fixed native INT8 mapping. The generic (`!forcedViewport`) branch fitted only
`displayW/H`; `fsrInputW/H` then used the still-zero neural target and reached
the 2x2 clamp. The corrected branch assigns the fitted dimensions to both the
display and neural target.

The campaign runner supplied `TFORGE_FSR4_FORCE_VIEWPORT` for all four source
tiers (360p, 480p, 720p, and 1080p), so those canonical campaign captures took
the forced branch. No historical campaign tier was affected by this defect.
The defect remains reachable in ordinary playback for non-fixed-target source
geometries, including arbitrary-aspect sources such as 1920x800. A production
build and a real temporal-pipeline run for an affected geometry are required
before this correction is promoted.

### CAS qualification

`PlaybackEngine.cpp` and `Fsr4DispatchHarness.cpp` use presence semantics:
`TFORGE_FSR4_DISABLE_CAS` is disabled only when `getenv()` returns null. The
historical precampaign runner set the variable to `"0"` for `cas20`, so that
arm was indistinguishable from no-CAS at runtime. The fix removes the variable
for `cas20`, clears inherited ambient state, and sets it only for `no_cas`.

This invalidates the historical CAS-enabled qualification claim and any gate
that depended on that specific qualification. It does not invalidate the
underlying frame/metric artifacts as observations, but CAS qualification must
be rerun on the corrected production head before a campaign is accepted.

The corrected runner was exercised through all 12 cases using a separate
two-scene fixture corpus at `/mnt/workdrive/tforge_precampaign_fixture_run3_20260906`.
All cases passed the automated gate. The six `no_cas` traces report
`cas_enabled=false` and `cas_stage=none`; the six `cas20` traces report
`cas_enabled=true`, `cas_stage=integrated_post_reconstruction`, and strength
`0.2`. This is direct runtime/tooling evidence for the presence-semantics fix,
not a replacement for rerunning the canonical corpus: the checked-in corpus
media available in this environment are Git-LFS pointer files and the original
manifest contains a machine-specific absolute path. The canonical CAS gate
therefore remains a required rerun when the real media are available.

### GPU timing

The historical canonical `fsr_pre.csv` files contain 220 rows; 189 have an
empty `gpu_ms_mean`. The per-route missing counts are:

```text
1080→1440 18/20   1080→2160 20/20   360→1080 18/20
360→480    9/20    360→720  15/20   480→1080 20/20
480→1440  20/20    480→720  12/20   720→1080 20/20
720→1440  19/20    720→2160 18/20
```

The images, quality metrics, hashes, and runtime traces are not thereby
discarded. These rows are **PERFORMANCE NOT QUALIFIED**. The supersampling
runner now has an explicit `--require-gpu-timing` performance gate that fails
when a `stage-timing` GPU sample is absent; ordinary quality capture remains
able to retain valid quality evidence without inventing timing.

### Contention provenance

The historical `PausingRunner` compatibility hook returned immediately and
never called its `games()` sampler. The capture policy itself is correct:
captures do not pause or terminate user processes. The corrected runner polls
and records matching process activity while each child capture runs. Historical
timing rows captured without that reliable observation are **PERFORMANCE NOT
QUALIFIED**, not invalid quality evidence.

### Campaign roots

The historical runner defaulted to the populated
`quality_campaign_capture_canonical_v1` and `review_harness_canonical_v1`
roots. Since that corpus was later marked historical/invalidated after the
lattice failure, the documented fresh execution path was misleading. Defaults
now allocate timestamped fresh roots; explicit `--resume` remains the only way
to reuse an existing root, subject to its provenance checks. Historical roots
remain untouched.

### Documentation authority

`M6_REGRESSION_TRIAGE.md` said M6 was open and prohibited M7 while the newer
progress state recorded the strict M6 gate closed and separately scoped M7.
The document is preserved under `docs/archive/plans/` as a historical triage
record. `docs/active/QUALITY_CAMPAIGN.md` remains the active Quality Lab
authority.

### Audio-only EOF

The reported audio-only EOF path predates the Quality Lab diff and touches
general playlist playback. No Quality Lab file changed it, so it is excluded
from this integration and remains a separate future bug.

## Evidence classification

- Valid quality evidence: frame payloads and quality metrics whose dimensions,
  references, hashes, and runtime identity validate.
- Limited evidence: historical rows missing GPU timing or contention sampling.
- Invalid/superseded campaign: the published canonical 20260904 corpus after
  the visible lattice finding; its artifacts remain historical provenance.
- Performance-not-qualified: any timing claim relying on missing timing or
  unestablished uncontended state.
- Rerun required: CAS-enabled precampaign qualification and any future
  performance-gated capture using the corrected runner.

## Stack decision

The original local graph was rejected during adversarial review: its branches
inherited the entire historical evidence tree, making intermediate diffs tens
of thousands of files and not meaningfully reviewable. The replacement is the
smallest clean stack found that keeps each layer buildable and gives each PR a
dominant purpose:

1. runtime integration and assets;
2. capture, metrics, provenance, and validation tooling;
3. documentation authority and adjudication record;
4. the already-accepted motion-confidence fallback, separately.

The generic sizing fix belongs in layer 1. CAS/timing/contention/root fixes
belong in layer 2. The M6 move and evidence classifications belong in layer 3.
Historical captures and invalidated outputs remain on PR #1's immutable head;
they are not silently copied into the production stack.

The accepted motion-confidence fallback remains a separate final PR on top of
the corrected Quality Lab head. Adaptive learned-strength, motion-anchor,
timescale, and other competition/R&D changes are excluded.

The production review stack is (heads recorded 2026-09-06, after the
independent audit/fix pass; the earlier `codex/*` graph was reconstructed
onto the `quality-lab/*` branches because the original branches had
duplicated lower-layer commits and stale stacked bases):

```text
main  45272e22
└─ quality-lab/runtime-reviewable        a1b9e119 + review fixes  0a49611d8
   └─ quality-lab/tooling-reviewable                              625bae68
      └─ quality-lab/docs-reviewable        (this branch, dated head)
         └─ quality-lab/confidence-fallback-final  (final layer, dated head)
```

The approximate clean edge sizes are 15.6K additions/93 files for runtime,
24.1K additions/128 files for tooling, and 1.7K additions/8 files for
documentation. The fallback edge is 156 additions/8 deletions in five files
(the caller-boundary sanitizer plus its regression tests and policy doc).
The production stack deliberately excludes generated capture payloads and
historical evidence dumps; those remain available from immutable PR #1.
The reviewable replacement PRs are #3 (runtime), #4 (tooling), #5
(documentation), and #6 (motion-confidence fallback). None is merged.

## Validation record

The corrected runtime isolated worktree built successfully with the repository's
existing FidelityFX SDK and miniaudio dependencies supplied from the local
device checkout (neither dependency link is part of the commit). CTest passed
100% of all runnable tests; the optional tensor-map test skipped because
the external tensor-map file is not present, and four GPU/diagnostic tests are
disabled by design. The tooling layer passed 44/44 focused Python contract
tests, including real child-process game-activity logging and executable
success/failure checks for the CAS and GPU-timing gates.

The real Temporal Forge pipeline was exercised with a generated 1920x800
source, with no forced viewport. The capture completed and produced normal
fitted dimensions (`854x356` model and `1280x534` output), directly exercising
the generic-target path; the retained run record is under
`/mnt/workdrive/tforge_arbitrary_run_runtime2_20260906`. The validation
artifacts are temporary and are not campaign evidence. The fallback layer's
focused CTest passed its one
registered `motion_estimator_tests` test. All four isolated worktrees were
clean after validation apart from the intentionally temporary dependency links,
which were removed and are not tracked.

Addendum (2026-09-06, independent audit/fix pass on the `quality-lab/*`
stack): runtime rebuilt fresh and CTest again passed all runnable tests
(19/19, one SKIP-77, four disabled by design) with a real RDNA3 headless FSR4
run producing a 1920x1080 output frame; the run used the generated
cooperative-matrix graph because the gitignored native INT8 shader pack was
absent from the clean tree, and that backend identity is recorded rather than
assumed. The tooling suite on a clean checkout is fully green (264 passed,
28 intentional artifact-dependent skips, 0 failed/0 errors) after the review
fixes for warmup alignment, dense-flow consistency domain, fail-closed
configs, and clean-checkout test hygiene. The fallback layer gained
caller-boundary sanitization of non-finite `TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE`
values with explicit nan/inf/-inf/malformed regression tests.
