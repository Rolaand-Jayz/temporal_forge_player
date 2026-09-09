# Third-party licenses and material inventory

This inventory lists tracked repository material that originates wholly or
partially outside Temporal Forge, based on repository evidence (embedded
license/SPDX statements, provenance READMEs, and build references). It is a
provenance inventory, not a legal opinion.

Temporal Forge's original code and documentation are licensed under the
Apache License, Version 2.0 (see `LICENSE`). Third-party material keeps its
own licensing; inclusion in this repository does not relicense it.

| Component / material | Repository paths | Origin | License | Treatment |
| --- | --- | --- | --- | --- |
| Khronos Vulkan headers (bundled header shim) | `external/vulkan_include/` | Khronos Group Vulkan SDK headers | Apache-2.0 OR MIT (SPDX in each header, e.g. `vulkan/vk_platform.h`) | Redistributed upstream source; existing SPDX/copyright notices preserved; not relicensed |
| miniaudio | `external/miniaudio.h` | David Reid, miniaudio v0.11.25 (2026-03-04), https://github.com/mackron/miniaudio | Public domain OR MIT No Attribution (both statements embedded in the header) | Redistributed upstream source; embedded license preserved; provenance in `external/README.md` |
| AMD FidelityFX FSR1 EASU math (build-time include) | `shaders/fsr1/fsr1_easu.comp` (entry point); includes `ffx_core.h` / `ffx_fsr1_easu.h` from the FidelityFX SDK, which is **not tracked** (opt-in via `TFORGE_ENABLE_FSR1_PROBE=ON` and provisioned under `external/FidelityFX-SDK/`) | AMD FidelityFX SDK (MIT-licensed SDK); the `.comp` entry point and descriptor setup are Temporal Forge-authored | SDK: MIT; entry point: Apache-2.0 as part of the project | No SDK material is redistributed in-tree; the SDK is a documented optional build dependency (`README.md`, `external/README.md`) |
| Generated FSR4 model source (referenced, not tracked) | referenced by `resources/fsr4/native_i8/*/README.md` | Generated HLSL in `Rolaand-Jayz/fsr4-rdna3-optimization` @ `49015b7`; that pack documentation states the generated source is copyright Advanced Micro Devices, Inc. | AMD copyright asserted by the repository's own pack documentation; no AMD license grant evidenced in-tree | **Not tracked here.** Referenced only as provenance; see `PROVENANCE.md` |
| Native INT8 FSR4 pack data (initializers, hashes, overrides) | `resources/fsr4/native_i8/*/initializers.bin` (12 files), `*/pack.sha256` (6), `performance_4320/workgroup_overrides.txt` | Reverse-engineering-derived artifacts generated from the AMD-copyright generated model source above | AMD copyright asserted over the source implementation; rights over the derived binary data are **not established** by repository evidence | **UNRESOLVED / PROVENANCE HOLD.** Committed (redistribution currently occurs). Not licensed under Apache-2.0; not removed; detailed in `PROVENANCE.md` |
| Generic FSR4 weight blobs | **not tracked** (provisioned externally at runtime) | Extracted from the AMD FSR 4.1.0 binary via the RE workflow (`Rolaand-Jayz/RE-of-FSR-4.1.0-Upscaling`) | Proprietary-derived; no license grant evidenced | **Not redistributed** — loaded at runtime from `TFORGE_FSR4_RE_ROOT` or the user's XDG data directory; see `PROVENANCE.md` |

All other tracked content (`src/`, `shaders/fsr4/`, `shaders/fsr1/`
entry-point file itself, `cmake/`, `cmake/stubs/`, `tools/`, `benchmarks/`,
`tests/`, `config/`, `docs/`, `resources/qml/`, `resources/icons/`,
`review_harness/`, root documents) is original Temporal Forge work unless a
file states otherwise, and is covered by `LICENSE`.

Per-file SPDX identifiers are intentionally not blanket-applied: the root
`LICENSE` grants by default, third-party files already carry their own
identifiers, and third-party identifiers are never replaced.

## Unresolved items

* The rights status of the committed native INT8 pack data
  (`resources/fsr4/native_i8/*/initializers.bin` and companions) cannot be
  established from repository evidence and is held under
  **PROVENANCE HOLD** — see `PROVENANCE.md`. No license claim is made for it.
