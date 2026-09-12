# Plan: Maintainability refactor + full function documentation

User chose: **Full reorganization** + **plain // comments** on every function.
Driven by the `production-refactor` skill: small steps, tests green after each,
no behavior changes, no stubs.

## Scope / TL;DR

Turn the codebase into a maintainable, well-organized, fully-documented project.
Remove the small amount of real cruft, split the two oversized files, reorganize
the directory layout into clear layers, add missing tooling/architecture docs,
and document EVERY function with a plain `//` block stating: what it does, what
calls it, what it calls, and non-obvious info (threading, ownership, preconditions).

The codebase is actually well-architected already. The two real problems are
(1) two oversized files and (2) uneven function-level comments.

## HARD CONSTRAINTS (must preserve — from repo memory regression lessons)

- PlaybackEngine threading model: decode thread does synchronous
  `vkWaitForFences(UINT64_MAX)` per frame in `Fsr4DispatchHarness::dispatchFrame`.
- `fsrDispatchMutex_` serializes UI-thread teardown vs decode-thread dispatch ONLY.
- Render-thread accessors `fsr4NativeOutput()` / `fsr4RawOutput()` must NOT take
  the dispatch mutex (doing so serialized 60Hz render against 5ms dispatch = stutter).
- Teardown safety for the render thread comes from `vkQueueWaitIdle()` in
  `teardownFsr4Path()` retiring in-flight GPU work.
- `setFsrViewport()` must only flip `fsr4Ready_` when the SCALE/preset ratio
  changes, NOT on pure window resize (teardown-on-resize caused multi-second reloads).
- QML enum gotcha: enums on a context-property instance cannot be referenced as
  `TypeName.EnumValue` from QML — use integer literals.

Any code move across files MUST preserve these exactly.

## Comment convention (plain `//` — matches existing file-header style)

Every non-trivial function gets a block above it:

```text
// <FunctionName>: <one-line what it does, beyond restating the name>
//
// Called by: <callers — e.g. "QML (Q_INVOKABLE)", "Qt render thread @ ~60Hz",
//                    "videoDecodeLoop() when fsr4Enabled_", or "internal helper">
// Calls:     <key callees — GPU/FFmpeg/mutex/atomic ops that matter>
// Notes:     <threading/ownership/preconditions/side effects — anything non-obvious>
```

Trivial one-line accessors (getters) get a single `//` line when context is
obvious, but `Q_INVOKABLE` and any function touching GPU/locks/threads always
gets the full block.

## Phase 0 — Baseline, tooling, architecture doc, cruft removal (no logic changes)

- [x] Green baseline: `cmake --build build` + `ctest --test-dir build` → 9/9 pass.
- [x] Remove cruft: top-level `temporal_forge_player/` (empty `shaders/fsr4/`) deleted.
- [ ] `.clang-format` — derive from existing, minimal diff.
- [ ] `.clang-tidy` — config only, no gate this round.
- [ ] `Doxyfile` — optional HTML browsing.
- [ ] `ARCHITECTURE.md` — layers + threading model + cascade.
- [ ] `CONTRIBUTING.md` — build/test + comment convention + invariants.
- [ ] Verify rebuild + tests still green.

## Phase 1 — Split `Fsr4DispatchHarness.cpp` (~3000 lines)

Public API (`Fsr4DispatchHarness`) stays IDENTICAL. Only internal helpers move
into `render/fsr4/`. Document every function as extracted. Verify build +
fsr4 tests.

## Phase 2 — Split `GpuImageUploader.cpp` (~1700 lines)

Public API (`GpuImageUploader`) stays IDENTICAL. Extract free-function helpers
into `render/upload/`; keep stateful methods grouped with section banners.
Document everything. Verify build + file_switch/frame_identity tests.

## Phase 3 — Backend + render directory reorganization

Keep flat (lower risk for CMake GLOB) but add `backend/README.md` and
`render/README.md` mapping each file to its role/tier.

## Phase 4 — Systematic per-function documentation (lowest-risk first)

1. `util/` 2. `audio/` 3. `config/` 4. `media/` 5. `ui/` 6. `backend/`
7. `render/` 8. `core/PlaybackEngine` (LAST, most careful) 9. `main.cpp`.

## Phase 5 — Final verification + doc sync

Clean build, full `ctest`, headless smoke, grep audit, README/ARCHITECTURE sync.

## Decisions (all locked)

- DOC FORMAT: plain `//` blocks (user choice); Doxyfile only for optional HTML.
- REORG DEPTH: full (user choice).
- Public class APIs FROZEN during splits (Phases 1-2).
- `gpu_probe` / `cm_dump` KEPT (intentional disabled GPU diagnostics).
- `tools/build_native_int8_pack.sh` KEPT (referenced by resources + benchmarks).
- CLANG-FORMAT: derive-from-existing, minimal diff.
- CLANG-TIDY: config only, no `WarningsAsErrors` this round.

## Tracked follow-up milestones (NOT in scope)

1. clang-tidy cleanup pass until the tree is clean enough to arm the gate.
2. clang-format changed-lines pre-commit hook after Phase 0 stabilizes.
