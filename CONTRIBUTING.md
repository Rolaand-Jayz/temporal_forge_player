# Contributing

A short guide for working in this codebase. Read `ARCHITECTURE.md` first for the
layered structure and the threading model.

## Build & test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

There are 9 active tests and 3 intentionally-disabled GPU diagnostics
(`gpu_probe`, `cm_dump`, `fsr4_harness_tests` — disabled because they need a
live Vulkan device and real FSR4 weights).

### Headless smoke test

```sh
timeout --signal=TERM 8 ./build/temporal_forge_player benchmarks/video_corpus/clips/<clip>.mp4
```

Exit code 124 is the timeout (expected). Look for `pipelineCPU=` lines to confirm
FSR4 dispatches are happening (3–4 in 8s for a 1080p→4K encode at 24fps).

### QML edits

The binary embeds QML via RCC + qmlcache. If `cmake --build` says "up to date"
but the QML change isn't reflected at runtime, force a full recompile:

```sh
touch CMakeLists.txt && cmake --build build
```

## Code style

- **Language:** C++23, `-Wall -Wextra -Wpedantic` (see `CMakeLists.txt`).
- **Formatting:** `.clang-format` is committed (4-space indent, K&R braces,
  left pointer alignment, ~80 col). It is derived from the existing style to
  keep diffs minimal — **do not reformat files you aren't otherwise editing.**
- **Linting:** `.clang-tidy` is committed as advisory config (no gate yet).
  You may apply trivially-safe auto-fixes (`clang-tidy -p build --fix`) on a
  file you're already editing, but never accept a fix that changes a signature
  or touches GPU/resource-lifecycle code.

## Documentation convention (plain `//`)

Every non-trivial function gets a `//` block immediately above it:

```cpp
// teardownFsr4Path: stop the decode loop's FSR4 dispatch, wait for the Vulkan
//                   queue to drain, then free the harness + uploader.
//
// Called by: setFsr4Enabled(false), close(), setFsrViewport (only on preset
//            ratio change). UI thread, while the decode thread may be mid-dispatch.
// Calls:     vkQueueWaitIdle, resets fsr4Uploader_/fsr4Harness_.
// Notes:     Holds fsrDispatchMutex_ so a dispatch in flight either completes
//            its queue submit (retired by the wait-idle) or hasn't started.
//            The render-thread accessors do NOT take this mutex.
void teardownFsr4Path();
```

- **What it does** — one line, beyond restating the function name.
- **Called by** — concrete callers (e.g. "QML via Q_INVOKABLE",
  "Qt render thread @ ~60Hz", "videoDecodeLoop when fsr4Enabled_").
- **Calls** — key callees: GPU/FFmpeg/mutex/atomic operations that matter.
- **Notes** — threading, ownership, preconditions, side effects.

Trivial getters get a single `//` line. Any `Q_INVOKABLE` and any function
touching GPU/locks/threads always gets the full block.

## Hard constraints (do not break these)

These invariants exist because breaking them caused recorded playback
regressions. If a change must touch one of them, add a test first.

1. `fsrDispatchMutex_` serializes UI-thread teardown vs decode-thread dispatch
   **only**. The render-thread accessors `fsr4NativeOutput()` /
   `fsr4RawOutput()` must never take it.
2. Teardown safety for the render thread comes from `vkQueueWaitIdle()` in
   `teardownFsr4Path()` retiring in-flight GPU work.
3. `setFsrViewport()` only flips `fsr4Ready_` when the preset **scale** changes,
   not on pure window resize.
4. The decode thread does synchronous `vkWaitForFences(UINT64_MAX)` per frame;
   stopping it cleanly means letting the current dispatch finish.

## What is NOT junk

Before deleting anything, confirm it isn't referenced:

- `tools/build_native_int8_pack.sh` — used by `resources/fsr4/*/README.md` and
  `benchmarks/video_corpus/RESULTS.md`.
- `tests/gpu_probe.cpp`, `tests/cm_dump.cpp` — disabled but intentional
  opt-in GPU diagnostics.
- `docs/FSR4_RE_STATUS.md` — the current FSR4 reverse-engineering log.
- `external/` headers are gitignored on purpose (re-vendored per build host).

## Refactoring safety

Follow the `production-refactor` rules: small steps, tests green after each,
no behavior changes, no stubs. See `docs/REFACTOR_PLAN.md` for the current
refactor in progress.
