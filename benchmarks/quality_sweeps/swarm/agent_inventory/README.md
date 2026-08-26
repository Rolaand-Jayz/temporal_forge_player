# Quality-control inventory

Inventory date: 2026-08-24. This is a static inventory of the current checkout. No player process, GPU capture, benchmark image, or source/default file was run or changed for this inventory.

The machine-readable files are:

- `quality_knobs.json`: valid JSON/configuration knobs, accepted values/ranges, defaults, and source evidence.
- `env_inventory.csv`: exact environment names, whether the player reads them, and whether the sweep scripts forward them.
- `cli_inventory.csv`: exact benchmark-script CLI names and their owning script.
- `evidence_files.txt`: inspected files, including the current build/config surfaces and sweep manifests.
- `manifest.md`: pre-existing companion inventory in this directory, retained unchanged.

## Findings

The primary quality configuration is `qualityLab` JSON, loaded by `src/config/QualityLabConfig.cpp`. It can be supplied with `TFORGE_QUALITY_LAB_CONFIG`; missing, malformed, or wrong-typed values retain typed defaults, and numeric values are clamped to the ranges recorded in `quality_knobs.json`.

The checked-in sweep candidates exercise composition, base filter, residual strength, tone exposure, adaptive sharpen, and presentation filter. The current swarm manifests are under `swarm/agent_a`, `swarm/agent_b`, `swarm/agent_e`, and `swarm/agent_e2`. Their configs are candidate inputs, not proof that captures were executed or approved. The pre-existing `manifest.md` records the broader stage sweep ranges and is retained as corroborating evidence.

The user-facing settings surface is separate from `qualityLab`: `SettingsStore.hpp` defines backend, preset, sharpness, jitter, motion, depth, reactive, presentation-scaler, and color controls. `TFORGE_BENCHMARK_PRESET`, `TFORGE_BENCHMARK_SHARPNESS`, and `TFORGE_BENCHMARK_JITTER_STRENGTH` override only benchmark-loaded settings in `src/main.cpp` and are not persisted.

The FSR4 runtime exposes additional experimental controls. The quality-relevant ones include forced viewport/scale, DRS, chain-pass count, jitter/reset policy, display-base/current composition controls, recurrent/color history, learned strength, CAS/RCAS, and implementation-path switches. Their accepted values are code-derived where parsing/clamping exists; presence-only switches are recorded as `presence=1`.

The sweep runners intentionally forward only selected environment names. A name read by the player but absent from the runner forwarding list is not a valid knob for an isolated sweep unless the caller changes the runner contract. Diagnostic, timing, dump, motion/event sidecar, and output-path variables are included in `env_inventory.csv` only when they are part of the benchmark contract, and are marked non-quality.

`CMakeLists.txt` has no user-selectable quality options. It defines the C++23/Release default, compiles the shader list, and builds the player/tests. `TFORGE_SOURCE_ROOT` and dependency/cache variables are build/configuration plumbing, not quality knobs.

## Current-worktree boundary

At inspection time the worktree was dirty before this inventory, with tracked edits in video-corpus scripts, FSR4/render/test files and untracked `benchmarks/quality_sweeps/swarm/` outputs. Those files were not edited. Only this directory was written.
