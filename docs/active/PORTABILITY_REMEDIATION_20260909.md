# Active plan: clean-clone portability / reproducibility / slop remediation

**Status:** VERIFICATION IN PROGRESS — findings being verified against live tree

**As of:** 2026-09-09

**Scope:** remediation of the 2026-09-09 initial portability/slop audit, with
independent audit expansion, three consecutive remediation-free clean loops,
and handoff to the independent adversarial verifier (Luna).

**Governing input:** initial audit
`20260909_TEMPORAL_FORGE_PORTABILITY_SLOP_INITIAL_AUDIT.md` (maintainer's
Downloads copy; verdict FAIL — REMEDIATION REQUIRED). The audit's Sections 11
(GLM remediation contract), 12 (three-consecutive-clean-loop DoD), 13 (Luna
post-remediation contract), and 14 (final campaign DoD) are authoritative for
this campaign and override generic process defaults.

## Branch strategy

- Audited public state: `main` @ `2d8eccbc12077b61da34290b27cd02ac68ad0e47`.
- Live development line: `quality-lab-vibecoder` @ `db19cfb34` (long-running
  quality-campaign branch; public `main` advances via PR merges from
  "reviewable" slices of this line).
- Remediation branch: `portability/clean-clone-remediation`, worktree
  `/mnt/workdrive/ZCodeProject/tf-portability-remediation`, based on
  `db19cfb34`.
- Delivery: fixes ride the established quality-lab line → PR → `main` path so
  the eventual public/default-branch candidate receives them (audit §11.2).
  The maintainer's uncommitted in-flight work in the main worktree is left
  untouched.
- Clean-tree validation runs from a fresh clone/export of the remediation
  branch into an arbitrary path, never from either maintained worktree.

## Mandatory independence rule (audit §11.1)

The initial audit is known-incomplete by design. While remediating its
findings, an independent full semantic/programmatic audit of the repository is
performed and every newly discovered defect is added to the ledger below.
Search tools build candidate maps only; findings are certified by tracing
source/build/runtime semantics.

## Clean-loop protocol (audit §12)

A qualifying loop performs the complete audit again: (A) semantic source audit
with coverage ledger, (B) mechanical corroboration (path scans, env-var
inventory, dependency inventory, untracked-asset checks), (C) clean-tree
reproducibility test from an arbitrary new path with a temporary HOME/XDG
where practical, (D) configure/build/ctest using only documented dependencies,
(E) runtime smoke (launch, known video, baseline/spatial path usable without
optional FSR4 capability, accurate absence errors), (F) runtime filesystem
audit (strace or equivalent proving no maintainer-specific opens), (G)
active-tooling smoke (representative runners resolve documented player build
and portable outputs).

**Counter semantics (strict):**

- A loop counts only if NO remediation is required as a result of that loop.
- If a loop discovers an actionable defect: finding → remediation →
  **counter resets to 0**, and the sequence restarts.
- The same loop in which a repair is made can never count as a clean loop.
- Remediation is complete only after **3 consecutive complete,
  remediation-free loops after the final change**.

| Loop | Date | Scope executed (A–G) | Verdict | Counter after loop |
|---|---|---|---|---|
| 1 | 2026-09-09 | A: fresh-eyes semantic sweep (full coverage ledger); B: mechanical path/deps sweeps; C+D: clean tree @ /tmp/tforge-portability-loop1 (temp HOME/XDG) configure+build+ctest 22/22; E: live Wayland runtime smoke — player launched, played, absence diagnostics fired verbatim, EASU fallback engaged; F: LD_PRELOAD open-logger (strace unavailable, no passwordless sudo) — 217 opens, zero maintainer-project paths; G: tooling --help/syntax/required-arg smokes | **DEFECTS FOUND — 7 actionable (N-1..N-6, N-D1 below) → remediated → loop does not count** | **0** |
| 2 | 2026-09-09 | A: lifecycle-first semantic audit (clone→launch walk, new-commit line-by-line scrutiny, fresh sweep methods: temp-collision patterns, spaces-in-path, end-to-end script reads); B: broadened mechanical sweeps (all evidence classes re-classified; env inventory 223 TFORGE_* consistent with documented contract); C+D: aborted mid-run by host /tmp exhaustion (infrastructure, not a repo defect; /tmp reclaimed; loop-1 evidence preserved under .campaign-portability/); E/F/G: not reached | **DEFECTS FOUND — 5 actionable (F1..F5 below) → remediated → loop does not count** (F6 adjudicated historical/acceptable) | **0** |
| 3 | 2026-09-09 | A: orchestrator-executed adversarial audit (worker pool usage-limited until 19:34; recorded honestly) — attacked all six campaign claims concretely: three independent git-archive builds prove dependency closure; test guards exception-safe; Fsr4Paths degrades on /proc failure/symlink/relative invocation; fresh mechanical sweeps clean; self-added doc claims verified (FFmpeg floor now enforced; stub env var covered by pointer-model contract); Vulkan gate fail-closed with live 14-feature log. B: machine-path scan in remediation tree — only adjudicated classes remain. C+D: clean tree @ /mnt/workdrive/tf-portability-loops/loop3 (temp HOME/XDG/CACHE) — configure+build green, ctest 22/22, deterministic skips. E: foreign-CWD live Wayland smoke — 14-feature device init incl. storageBuffer8BitAccess, blob-absence diagnostic, EASU fallback engaged (external-build-dir layout loads default lab config, accurately logged — documented observation). F: LD_PRELOAD audit, corrected counting — 217 opens, ZERO maintainer-project opens. G: py_compile/bash -n/required-arg/binary-strings smokes green | **CLEAN — remediation-free** | **1** |

## Findings ledger

Verdicts are from verification against live tree `db19cfb34` (workers V1–V3:
build/dependency contract; runtime resources; Vulkan/device/docs). Disposition
and validation are recorded as remediation proceeds. Historical evidence
provenance is preserved; no mass path sanitization (audit §8).

| ID | Severity | Summary | Verified | Disposition | Validation |
|---|---|---|---|---|---|
| C-01 | CRITICAL | Clean clone missing required `miniaudio.h` | HOLDS (V1): AudioSink.cpp:5 include; external/ on public include path; .gitignore:16; absent in clean tree; no provisioning docs | vendored tracked dependency with provenance/version | build succeeds in clean tree |
| C-02 | CRITICAL | Default build compiles FSR1 probe requiring gitignored FidelityFX SDK | HOLDS (V1): CMakeLists.txt:83-86 unconditional; fsr1_easu.comp:19,59 ffx includes; ShaderCompile.cmake:42-45 SDK include paths; no option() anywhere | make probe opt-in, disabled unless SDK deliberately provisioned | clean-tree configure+build without SDK |
| C-03 | CRITICAL | Native FSR4 resource packs incomplete (no `passN.spv` in tree) | HOLDS (V2): Fsr4DispatchHarness.cpp:811-818 loads TFORGE_SOURCE_ROOT-based passN.spv (graceful false on missing); :1194-1224 requires exactly 89216-byte initializers.bin; .gitignore:8 `*.spv`; tracked = READMEs/initializers only; pack builder not wired into build | documented provisioning contract; portable asset-root resolution (exe-relative resources + env); diagnostics distinguish missing-shaders vs unsupported-pack; no silent dev-host coupling | clean-tree launch with actionable absence state |
| C-04 | CRITICAL | `fsr4_weight_tests` lacks CTest `SKIP_RETURN_CODE 77` | HOLDS (V1); extends to media_pipeline_tests (registered unconditionally, fixture needs ffmpeg+libx264+aac) and file_switch_tests pattern | proper skip semantics for external-data tests | ctest deterministic on clean host |
| M-01 | MAJOR | Developer-specific FSR4 weight lookup paths in runtime `PlaybackEngine` | HOLDS (V2): PlaybackEngine.cpp:1395-1447 precedence env→2×/mnt→/home/rolaandjayz→2× sibling-relative; tracked resources/ not among candidates; absence = one warn line, upscaling disabled | replace with documented precedence (env → XDG data → explicit unavailable state); remove maintainer paths | clean-tree launch: FSR4 unavailable state accurate; strace clean |
| M-02 | MAJOR | `TFORGE_SOURCE_ROOT` absolute build-host path used by runtime code | HOLDS (V2): CMakeLists.txt:13; runtime uses QualityLabConfig.cpp:131-135 (has fallback chain) + Fsr4DispatchHarness.cpp:812,1217 (NO fallback); test-only uses acceptable | drop source-root tier from all runtime resolution; portable runtime contract only | binary runs from arbitrary path/checkout deleted |
| M-03 | MAJOR | Declared build/runtime prerequisites materially incomplete | HOLDS (V1): glslangValidator REQUIRED (ShaderCompile.cmake:22) undocumented; Qt ShaderTools undeclared (README:22, ARCHITECTURE:145); Vulkan 1.3 runtime (VulkanContext.cpp:129) undocumented; ffmpeg+libx264/aac fixture undeclared | authoritative dependency matrix in README; CMake validates early | clean-host configure fails only with actionable messages listing exact deps |
| M-04 | MAJOR | Public backend/default documentation contradicts runtime defaults | HOLDS-worse (V3): SettingsStore.hpp:31,60 defaults Fsr4ReExperimental+allowExperimentalAsDefault=true vs README:55 "FSR 2.3 SDK — stable production path (default)"; TFORGE_HAVE_FSR3_SDK never defined → SDK backend compiled-out stub; three naming schemes (README vs UpscaleTypes.cpp:53 "FSR 2.3 SDK" vs BackendSelector.cpp:105 "FSR 3.1.5 (fallback)") | single truth: code default (FSR4-RE on supported hardware, spatial fallback) propagated to README/ARCHITECTURE/UI/docs; SDK stub labeled unavailable; naming unified | doc/code consistency check + ctest |
| M-05 | MAJOR | Vulkan device creation over-constrained before backend fallback | HOLDS (V3): VulkanContext.cpp:242-247 four unchecked extensions (no vkEnumerateDeviceExtensionProperties in file); ~10 unchecked feature enables (fp16/int8/dot-product/coop-matrix/subgroup-size-control); no retry at :300-303; main.cpp:63-66 hard-exits on Qt instance <1.3; isAmdRadv (VulkanContext.cpp:28-39) true for all 0x1002; spatial fallback unreachable when device creation fails | baseline device (swapchain + checked core features) for player/spatial; FSR4-class features probed separately, gate BackendSelector on capability; fix isAmdRadv via driver-properties; runtime-validated on local RDNA3 + synthetic incapability tests | clean-tree launch + new capability tests + local hardware smoke |
| M-06 | MAJOR | Active tooling defaults to `build-fast` binary convention | HOLDS (V1): 12 active files incl. run_lattice_diagnostic.sh (no override) and campaign_provenance.py; 3 use CWD-relative defaults (NEW-04) | centralize canonical build-path resolution; build-fast becomes explicit option | tooling smoke from clean tree using documented build/ |
| M-07 | MAJOR | Capture tooling defaults to maintainer's external-drive mount | HOLDS (V1): capture_review_best_finds.sh:13 `/mnt/external/...` default, env override undocumented; only remaining machine-path default in active scripts | required/portable default; document override | tooling smoke with clean env |
| M-08 | MAJOR | `docs/current/STATE.md` stale relative to branch it governs | PARTIAL (V3): audited "PRs #3-#6 not merged" text exists only on main @ 2d8eccbc1 (not ancestor of branch); live STATE.md self-declares CURRENT @ 2026-09-02/source 269631d vs branch reality 2026-09-05+ — stale either way | refresh STATE.md to current truth on remediation branch (rides PR to main) | docs review in clean loop |
| M-09 | MAJOR | Checked-in `quality_lab.json` startup role not stated cleanly | HOLDS (V2): main.cpp:86-88,120 unconditional load; config enabled=true base_only/bilinear; PlaybackEngine.cpp:1537-1552 scale-aware policy (disabled <3x unless TFORGE_QUALITY_LAB_CONFIG set); tests codify as measured default; STATE.md:24-26 in tension | ADJUDICATED: checked-in config is the shipped measured policy (incl. scale-aware ≥3x behavior); runtime behavior preserved (quality baseline); docs must state this explicitly instead of "diagnostics do not silently define the normal player path" | docs/code consistency check |
| m-01 | MINOR | RX 7900 GRE queue policy generalized as global policy | HOLDS (V2): main.cpp:143-149 comment cites single-GPU measurement; dedicated compute queue discovered/created but zero consumers | ADJUDICATED: behavior preserved (performance evidence; audit forbids casual change); policy documented as target-specific in docs | docs review |
| m-02 | MINOR | Contributor/test-count documentation stale | HOLDS-worse (V3): CONTRIBUTING.md:14-15 claims 9 active/3 disabled; actual 23 tforge_add_test + 1 conditional, 4 DISABLED (jitter_gpu_contract_tests omitted from docs) | correct counts + disabled list | docs review |
| m-03 | MINOR | Manual/historical tests retain fixed `/tmp` fixture locations | HOLDS-corrected (V2): test_m6_spatial_provenance.py:21 dated /tmp fixture; NOT in CTest/automation; does NOT skip — errors (FileNotFoundError) under pytest discovery | existence-guard so discovery is clean; classify as historical fixture validation | pytest discovery clean |
| H-01 | HOLD | No repository LICENSE declared | HOLDS (V3): no LICENSE/COPYING anywhere; no license sections in README/CONTRIBUTING; vendored Khronos headers carry Apache-2.0/MIT SPDX with no NOTICE file | maintainer decision required; NOT resolved during technical cleanup; NOTICE/attribution recorded as dependency-matrix obligation | recorded for public-readiness campaign |

### Independently discovered findings (audit §11.1)

**Wave 1 implementation status (2026-09-09):** implemented and build-verified
(configure → build → ctest green in-worktree): C-01, C-02 (with documented
no-op-stub deviation for the GpuImageUploader unconditional-include coupling),
C-04, M-03 (dependency matrix), M-04 (docs truth + label unification), M-06
(campaign_provenance.py adjudicated: no default exists — build-fast strings
are manifest schema provenance fields, historical evidence preserved), M-07,
M-08, m-02, GLM-NEW-02/03/06. Deferred follow-up recorded: compile-time gating
of the true-FSR1 pipeline in GpuImageUploader (src-side cleanup). Per-row
Validation evidence accrues during the clean loops below.

| ID | Severity | Summary | Evidence | Disposition | Validation |
|---|---|---|---|---|---|
| NEW-01 | low | `find_program(NINJA_EXE ninja REQUIRED)` forces Ninja even for deliberate non-Ninja generators | CMakeLists.txt:23 (V1) | adjudicate: document as supported-generator constraint or relax | configure smoke |
| NEW-02 | info | Bundled Vulkan headers FORCE-override user `-DVulkan_INCLUDE_DIR` | CMakeLists.txt ~40-46 (V1); headers tracked, so bundling itself is a portability improvement | informational; keep bundling, note override semantics | none needed |
| NEW-03 | medium | `jq` and `magick` hard-required by capture_review_best_finds.sh, undocumented (script fails fast with clear message) | capture_review_best_finds.sh:20-21 (V1) | add to dependency matrix; sweep other scripts' tool guards | tooling smoke |
| NEW-04 | low-med | CWD-relative `build-fast/...` defaults in run_harness_campaign.py:466, run_fsr_supersampling.py:282, run_baseline_qualification.py:79 (break when invoked off repo root) | V1 | fixed with M-06 centralization | tooling smoke from foreign CWD |
| NEW-05 | low | `pkg_check_modules(FFMPEG_TEST ...)` unguarded; `PkgConfig::FFMPEG_TEST` assumed to exist | tests/CMakeLists.txt media block (V1) | guard target creation on result | configure smoke |
| NEW-06 | info | `file_switch_tests` registered with duplicated sample.mp4 arg + unregistered 77 skip (currently ffmpeg-gated) | tests/CMakeLists.txt EOF; file_switch_tests.cpp:73,77 (V1) | fold into C-04 skip-semantics fix | ctest on clean host |
| GLM-NEW-01 | withdrawn | ~~`external/vma.h` gitignored hidden dependency~~ — NOT a build dependency: zero usage in src//CMake/cmake (verified by grep + worker sweep); unused host-local file | .gitignore:15; usage grep empty | no action; keep ignored | n/a |
| GLM-NEW-02 | low | `external/vulkan_include/` tracked bundled Vulkan-headers shim undeclared in docs | CMakeLists.txt:43,119 | fold into M-03 dependency matrix | clean-tree build |
| GLM-NEW-03 | MAJOR | Qt Vulkan instance hard-requires API 1.3, process exits before any fallback (compounds M-05) | main.cpp:63-66 (V3) | document Vulkan 1.3 as hard requirement in matrix (device-level fix handled by M-05) | docs + clean-tree launch |
| GLM-NEW-04 | MAJOR | Unconditional `requiredSubgroupSize = 64` (wave64) in FSR4 pipeline creation | Fsr4DispatchHarness.cpp:839-850 (V3) | gate on probed subgroup-size-control capability (with M-05) | capability tests |
| GLM-NEW-05 | low | GpuCapabilityProbe gates RDNA on marketing-name substrings + duplicate device-scoring vs VulkanContext | GpuCapabilityProbe.cpp:40-43,193,47-67 (V3) | context-side driver detection fixed (M-05); probe is diagnostic tooling — name-matching limitation documented, refactor deferred | docs review |
| GLM-NEW-06 | medium | Backend label drift: enum/UI "FSR 2.3 SDK" for an FSR 3.1.5 implementation vs selector "FSR 3.1.5 (fallback)" | UpscaleTypes.cpp:53; Fsr3FallbackBackend.cpp:36; BackendSelector.cpp:105 (V3) | user-facing labels unified to "FSR 3.1.5 (SDK)" (UpscaleTypes, BackendSelector, FsrController comment). ADJUDICATED: Fsr23SdkBackend.cpp keeps its truthful "FSR 2.3 SDK" name — it wraps ffx_fsr2 and is dead code (never instantiated; BackendSelector uses Fsr3FallbackBackend only); removing it is out-of-scope refactoring | docs/UI consistency check |
| GLM-NEW-07 | low | VK_EXT_DEBUG_UTILS instance extension added without availability check | VulkanContext.cpp:101-110 (V3) | enumerate before adding (with M-05) | clean-tree launch |
| GLM-NEW-08 | medium | 196 undeclared `TFORGE_*` env knobs in production src/, some re-read in render hot path | V2 sweep: 11 files; concentrations PlaybackEngine.cpp, Fsr4DispatchHarness.cpp, main.cpp | ADJUDICATED: intentional runtime-configurable experiment contract (AGENTS.md working rule 3 — no recompile to test values); remediation = documented contract section in reference docs; hot-path re-read hygiene deferred to quality phase | docs review |
| GLM-NEW-09 | low | SettingsStore falls back to CWD-relative settings file when HOME/XDG unset | SettingsStore.cpp:259-267 (V2) | document last-resort semantics in reference docs | docs review |
| GLM-NEW-10 | MAJOR | Generic FSR4 weight blob has no in-repo or portable candidate; tracked FSR4 artifacts unloadable — systemic gap with C-03/M-01 | WeightBlob.cpp (validation only); PlaybackEngine.cpp:1394 | folded into M-01 resolution contract | clean-tree unavailability state |
| GLM-NEW-11 | low | Pack table/graph mismatch (ultraperf_1080 ships no initializers) indistinguishable from provisioning gap in errors | Fsr4DispatchHarness.cpp:1194-1216 (V2) | folded into C-03 diagnostics | clean-tree absence state |
| N-1 (loop 1) | low | Machine candidate paths persist in test sources; do not honor TFORGE_FSR4_RE_ROOT despite the new runtime contract | tests/fsr4_weight_tests.cpp:20-27; tests/fsr4_harness_tests.cpp:96-104 (loop-1 audit) | tests honor the env override first, keep 77-skip | ctest + grep |
| N-2 (loop 1) | low | Shipped default quality_lab.json resolved CWD-dependently only; docs claim it loads at startup from any launch | src/config/QualityLabConfig.cpp:126 (loop-1 audit) | exe-relative tier added (env → exe-relative → CWD → XDG → HOME) | launch from foreign CWD smoke |
| N-3 (loop 1) | moderate | 16/8-bit storage-access features required by fsr4 shaders (GL_EXT_shader_16bit_storage) never enabled/probed — RADV leniency is load-bearing | VulkanContext.cpp:525-531; GpuCapabilityProbe.cpp:213-259; conv_dw_dot4.comp:9-20 (loop-1 audit) | added to planner FSR4-class enables + probe checks + hermetic tests; live-verified | capability tests + live init |
| N-4 (loop 1) | minor | Probe omits shaderIntegerDotProduct while planner counts it in fsr4ClassFeaturesPresent (claimed equivalence inexact; fail-safe direction) | VulkanContext.cpp:147 vs GpuCapabilityProbe.cpp:224-248 (loop-1 audit) | probe check aligned | capability tests |
| N-5 (loop 1) | minor | TFORGE_FSR4_TRUE_FSR1_EASU on a stub build silently dispatches no-op EASU (undefined intermediate) | GpuImageUploader.cpp:2301; cmake/stubs/fsr1_easu.comp (loop-1 audit) | stub builds define TFORGE_FSR1_PROBE_STUB; runtime warns once when the env var is set | build + grep |
| N-6 (loop 1) | minor | No minimum FFmpeg version declared; code needs ≥5.1-era APIs (ch_layout, AVFrame.duration) | CMakeLists.txt:35-37; README matrix (loop-1 audit) | matrix states FFmpeg ≥ 5.1 | docs review |
| N-D1 (loop 1) | minor | environment.md describes weight blobs as `v410_*.bin`; actual files are quality.bin/balanced.bin/... (v410_initializers is the directory) | docs/reference/environment.md:43 vs WeightBlob.cpp:21-27 (loop-1 audit) | corrected blob names | docs review |
| F1 (loop 2) | low | Fixed-name temp files in quality_lab_config_tests — collision/flake risk on shared machines | tests/quality_lab_config_tests.cpp:21,30 (loop-2 audit) | pid-unique names (pattern of fsr4_paths_contract_tests) | ctest |
| F2 (loop 2) | low | qualityLabConfigPath() tier chain (incl. the new exe-relative tier) has zero test coverage | src/config/QualityLabConfig.cpp:126 vs quality_lab_config_tests.cpp (loop-2 audit) | precedence tests for env/CWD/XDG/HOME/absence | ctest |
| F3 (loop 2) | low | README claims FFmpeg <5.1 fails configure; pkg_check_modules enforces no version — misleading failure mode | CMakeLists.txt:37-38 (loop-2 audit) | version floors in pkg_check_modules matching the 5.1 API surface | configure smoke |
| F4 (loop 2) | low | Unquoted shader paths in ShaderCompile break on checkouts/build dirs containing spaces | cmake/ShaderCompile.cmake:46-51 (loop-2 audit) | quote paths; spaced-path build probe added to loop gates | spaced-path build |
| F5 (loop 2) | low | Settings writer interpolates lastOpenDir unescaped into JSON — a `"` in the path silently corrupts settings.json | src/config/SettingsStore.cpp:250 (loop-2 audit) | escape backslash/quote in serialized string fields | ctest + settings unit check |
| F6 (loop 2) | adjudicated | Legacy maintainer candidates remain as fallbacks in external-data tests after the env tier | tests/fsr4_weight_tests.cpp:22-30; fsr4_harness_tests.cpp:97-104 | HISTORICAL/acceptable: absent on clean clones → 77 skip; env override is the portable entry; residue documented | n/a |

## Remediation principles (audit §11.3, binding)

Prefer: one canonical dependency mechanism; one canonical runtime resource
contract; one canonical build path; explicit required/optional dependencies;
capability-based feature gating; deterministic test fixtures; clean
failure/fallback semantics; clear public truth. Avoid: machine-specific
fallback paths; new magic environment variables where configuration exists;
duplicated path-resolution logic; silently skipping meaningful tests;
committing local build products as a shortcut; replacing historical
provenance; wrappers that hide broken assumptions.

## Final DoD

The §14 checklist (items 1–20) is copied into the completion report with
per-item evidence when the campaign closes. Item 16 (3 consecutive
remediation-free loops) and item 18 (Luna's 2 clean Devil loops) gate the
final verdict; Luna handoff package = initial audit + this ledger + clean-loop
reports + candidate commit/branch identity.

## Progress log

- 2026-09-09 (wave 3 committed `3a0fafff3`): M-05 remediated — device
  requests planned from enumerated evidence (pure `planVulkanDeviceRequests`
  layer + hermetic `vulkan_capability_plan_tests`); FSR4-class items probe
  separately and gate only the FSR4-RE backend via the existing
  GpuCapabilityProbe handoff, so an incapable device reaches the spatial
  fallback instead of dying at `vkCreateDevice`; isAmdRadv uses driver
  properties; wave64 and debug-utils requests are capability-gated.
  GLM-NEW-04/07 closed. Live validation: gpu_probe viable on RADV 26.2.2;
  non-Qt init smoke proved instance → 246 extensions enumerated → 5 enabled
  → device created with fsr4 caps viable; enabled set identical to pre-change
  plus the previously missing VK_EXT_subgroup_size_control. Binary strings
  clean. Implementation of ALL ledger dispositions is complete; clean-loop
  qualification begins (counter = 0).
- 2026-09-09 (wave 2 committed `37128fdc6`): runtime FSR4 asset resolution is
  host-independent. Shared resolver `src/util/Fsr4Paths` (exe-relative + CWD
  for native packs; `TFORGE_FSR4_RE_ROOT` + XDG data for weight blobs);
  `TFORGE_SOURCE_ROOT` compile definition removed (test targets define it
  per-target); `-ffile-prefix-map` keeps `__FILE__` diagnostics free of the
  build-host path (orchestrator-approved adjacent-scope addition). Absence
  diagnostics distinguish pack-missing/pack-incomplete/initializer-invalid;
  weight-blob absence lists searched paths and the override. New hermetic
  `fsr4_paths_contract_tests`; `test_m6_spatial_provenance` now skips without
  its dated fixture. Binary `strings` check clean of maintainer paths; ctest
  21/21 active tests pass. Live offscreen player run was blocked in the
  worker's GPU-restricted shell (validated via contract test + direct code-path
  execution instead) — live smoke is a clean-loop gate.
- 2026-09-09 (loop 1): full audit launched — fresh-eyes semantic sweep over
  the remediated tree plus mechanical corroboration, arbitrary-path clean-tree
  configure/build/ctest, runtime smoke (generated sample media), strace
  filesystem audit, tooling smoke.

- 2026-09-09: Campaign opened. Live state established: local line
  `quality-lab-vibecoder` @ `db19cfb34` (~140 commits ahead of audited public
  `main` @ `2d8eccbc1`; `main` advances via PR merges). Remediation branch
  `portability/clean-clone-remediation` created at `db19cfb34` in worktree
  `/mnt/workdrive/ZCodeProject/tf-portability-remediation`. Verification
  workers V1 (build/deps), V2 (runtime resources), V3 (Vulkan/docs) dispatched
  against the live tree with the §11.1 new-defect mandate.
- 2026-09-09: Verification complete. All findings verified against live tree;
  15 of 16 audited items stand (M-08 partial: audited text is main-only but
  live STATE.md stale differently; m-03 corrected: file errors rather than
  skips under pytest discovery; H-01 hold confirmed). V1/V2/V3 produced 19
  candidate new defects; after cross-checks and adjudication, 14 recorded
  above (GLM-NEW-01 withdrawn — vma.h has zero build usage). Remediation
  sequence planned: wave 1 = IMPL-1 (build contract: vendored miniaudio,
  opt-in FSR1 probe, ctest skip semantics, canonical tooling paths, machine
  mount default removal) parallel with IMPL-4 (documentation truth:
  dependency matrix, backend-truth propagation, STATE.md refresh, test counts,
  env contract); wave 2 = IMPL-2 (runtime FSR4 asset resolution contract);
  wave 3 = IMPL-3 (Vulkan baseline-device/capability probing). Waves sequenced
  by build dependency and Fsr4DispatchHarness.cpp write overlap.
