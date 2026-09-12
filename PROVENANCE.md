# Provenance

This document records where tracked material comes from when the origin
matters beyond ordinary package attribution — especially FSR4 /
reverse-engineering (RE) related artifacts.

Project-level boundary:

> Temporal Forge licenses its original implementation and documentation
> under Apache License 2.0 (`LICENSE`). This license applies only to rights
> the project's contributors possess authority to grant. Third-party
> components and reverse-engineering-derived artifacts retain their
> applicable provenance, licensing, and rights status as documented here and
> in `THIRD_PARTY_LICENSES.md`. Reverse engineering is a legitimate method
> of interoperating with and learning from shipped software; it does not by
> itself transfer or create rights in the underlying material.

Referenced external repositories are cited by identifier and commit rather
than machine-local paths:

* RE workflow repository: `Rolaand-Jayz/RE-of-FSR-4.1.0-Upscaling`
  (MIT, © 2025 Rolaand Jayz; referenced snapshot `5c1ff9a537e755c38e446f0b4eaa426a0000b3ed`).
* Model/HLSL generation repository: `Rolaand-Jayz/fsr4-rdna3-optimization`
  (referenced by the pack READMEs at commit `49015b7`).

---

## Native INT8 FSR4 pack data (tracked)

Paths:
`resources/fsr4/native_i8/<pack>/initializers.bin` (12 packs:
`native_2160`, `performance_2160`, `performance_4320`,
`performance_4x3_1440`, `performance_4x3_2880`, `quality_1080`,
`quality_2160`, `quality_4x3_1440`, `quality_4x3_2880`, `ultraperf_2160`,
`ultraperf_4x3_1440`, `ultraperf_4x3_2880`), plus `*/pack.sha256` (6) and
`performance_4320/workgroup_overrides.txt`. Per-pack `README.md` files
document each graph's shape and pairing rules; `ultraperf_1080` ships a
README only.

Origin:
Generated in `Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`
from FSR 4.1 model material recovered by reverse engineering. The
repository's own pack documentation states, for the generated model source,
that "the generated source is copyright Advanced Micro Devices, Inc." and
that compiled binary assets "preserve that implementation and are not
hand-written substitutes" (`resources/fsr4/native_i8/ultraperf_2160/README.md`).

Classification:
Reverse-engineered / generated (extracted-and-compiled model data).

Original / upstream OSS / generated / reverse-engineered / extracted:
Reverse-engineered, then generated: the tensor graphs and INT8 initializer
data are produced by the RE model pipeline from AMD FSR 4.1 model content,
not hand-written.

Redistributed in repository:
Yes — the `initializers.bin` files and companions are committed.

Upstream/source reference:
`Rolaand-Jayz/fsr4-rdna3-optimization` @ `49015b7`; RE methodology in
`Rolaand-Jayz/RE-of-FSR-4.1.0-Upscaling`.

Applicable license or rights status:
**UNRESOLVED / PROVENANCE HOLD.** Repository evidence establishes an AMD
copyright claim over the generated model source and no license grant from
AMD over the model data. The RE workflow repository's MIT license is the
maintainer's grant over their own authorship there and does not extend to
AMD-derived content.

Temporal Forge licensing treatment:
NOT licensed under the project's Apache-2.0 `LICENSE`. Retained in the
repository unchanged; no relicense, no removal, no invented legal
conclusion. The runtime treats these packs as optional data with explicit
provisioning diagnostics.

Reproduction/provisioning method:
`tools/build_native_int8_pack.sh` compiles the generated HLSL (from the
generation repository, staged locally) into SPIR-V with DXC at Vulkan 1.2 /
SM 6.6 and pairs it with the initializer; `tools/adapt_native_int8_hlsl.sh`
derives guarded 4:3 spatial specializations. Compiled `passN.spv` modules
are build products and are not tracked (`.gitignore` excludes `*.spv`);
the tracked artifacts are the initializers and their metadata.

Notes:
The runtime hard-validates pack pairing (initializer size 89216 bytes,
`pack.sha256` content addressing) and reports missing or mismatched packs
as actionable diagnostics rather than failing silently.

---

## Generic FSR4 weight blobs (not tracked)

Paths:
Not committed. Loaded at runtime from `TFORGE_FSR4_RE_ROOT` or
`$XDG_DATA_HOME/temporal-forge-player/fsr4/extracted/v410_initializers/`
(see `docs/reference/environment.md`); blob names and sizes are documented
there (`quality.bin`, `balanced.bin`, `performance.bin`, `ultraperf.bin`,
`native.bin`, `drs.bin`; 131072 bytes each).

Origin:
Extracted from the AMD FSR 4.1.0 binary by the RE workflow
(`Rolaand-Jayz/RE-of-FSR-4.1.0-Upscaling`).

Classification:
Reverse-engineered / extracted (direct binary extraction).

Original / upstream OSS / generated / reverse-engineered / extracted:
Extracted from proprietary upstream content.

Redistributed in repository:
No — deliberately provisioned by the user at runtime; the repository
contains none of this data.

Upstream/source reference:
`Rolaand-Jayz/RE-of-FSR-4.1.0-Upscaling` (extraction tooling and notes).

Applicable license or rights status:
Proprietary-derived; no license grant evidenced.

Temporal Forge licensing treatment:
Not licensed under the project's Apache-2.0 `LICENSE`. The player runs
without them (documented fallback to the spatial path) and logs an
actionable, accurate absence diagnostic naming the searched locations.

Reproduction/provisioning method:
Extraction tooling and instructions live in the RE repository; runtime
resolution and diagnostics are implemented by `src/util/Fsr4Paths` and
`src/core/PlaybackEngine.cpp`.

Notes:
Nothing in this repository ships the blobs; nothing here asserts rights
over them.

---

## AMD FidelityFX FSR1 probe (build-time include, not tracked)

Paths:
`shaders/fsr1/fsr1_easu.comp` (Temporal Forge-authored Vulkan entry point);
`#include "ffx_core.h"` / `#include "ffx_fsr1_easu.h"` resolve to the
FidelityFX SDK, which is **not tracked** (gitignored
`external/FidelityFX-SDK/`, opt-in via `TFORGE_ENABLE_FSR1_PROBE=ON`).

Origin:
The EASU math is AMD FidelityFX SDK source (MIT-licensed AMD open source).
The entry-point file, descriptor formats, and build wiring are Temporal
Forge-authored.

Classification:
Original implementation with a build-time dependency on permissively
licensed upstream source.

Original / upstream OSS / generated / reverse-engineered / extracted:
Upstream OSS (MIT SDK) included at build time only, when deliberately
provisioned.

Redistributed in repository:
No SDK header or source is committed. The compiled probe shader is a build
product, not tracked.

Upstream/source reference:
AMD FidelityFX SDK (MIT); see `README.md` dependency matrix and
`external/README.md`.

Applicable license or rights status:
MIT (SDK). MIT terms permit inclusion and require preservation of
copyright/license notices in redistributed SDK material — which this
repository does not redistribute.

Temporal Forge licensing treatment:
The authored entry point is covered by the project `LICENSE`. If SDK
material is ever vendored, it must keep its MIT notices and be added to
`THIRD_PARTY_LICENSES.md`.

Reproduction/provisioning method:
Configure with `-DTFORGE_ENABLE_FSR1_PROBE=ON` and stage the SDK under
`external/FidelityFX-SDK/sdk/include/`; configure fails with an actionable
message when the SDK is absent. Default builds compile a documented no-op
stub (`cmake/stubs/fsr1_easu.comp`) and warn if the research flag
`TFORGE_FSR4_TRUE_FSR1_EASU` is used against it.

Notes:
`shaders/fsr4/easu.comp` is a separate, Temporal Forge-authored spatial
fallback kernel and is not the FidelityFX FSR1 implementation.

---

## Khronos Vulkan headers (tracked, redistributed upstream source)

Paths:
`external/vulkan_include/` (vulkan/, vk_video/)

Origin:
Khronos Group Vulkan SDK headers.

Classification:
Upstream OSS.

Original / upstream OSS / generated / reverse-engineered / extracted:
Upstream OSS (redistributed).

Redistributed in repository:
Yes, as a bundled build-time header shim.

Upstream/source reference:
Khronos Group Vulkan SDK.

Applicable license or rights status:
SPDX-License-Identifier: Apache-2.0 OR MIT, declared in each header
together with Khronos copyright notices (preserved unmodified).

Temporal Forge licensing treatment:
Existing notices retained; not relicensed; listed in `NOTICE` and
`THIRD_PARTY_LICENSES.md`.

Reproduction/provisioning method:
Bundled; `CMakeLists.txt` prefers them over system headers and falls back
cleanly.

Notes:
None.

---

## miniaudio (tracked, redistributed upstream source)

Paths:
`external/miniaudio.h`

Origin:
David Reid, miniaudio v0.11.25 (2026-03-04),
https://github.com/mackron/miniaudio.

Classification:
Upstream OSS.

Original / upstream OSS / generated / reverse-engineered / extracted:
Upstream OSS (redistributed, unmodified single header).

Redistributed in repository:
Yes, verbatim (sha256 recorded in `external/README.md`).

Upstream/source reference:
https://github.com/mackron/miniaudio

Applicable license or rights status:
Public domain OR MIT No Attribution (choice embedded in the header with
full license statements, "Copyright 2026 David Reid").

Temporal Forge licensing treatment:
Existing statements preserved; not relicensed; listed in `NOTICE` and
`THIRD_PARTY_LICENSES.md`.

Reproduction/provisioning method:
Vendored; provenance recorded in `external/README.md`.

Notes:
None.

---

## Generated SPIR-V (build products, not tracked)

Paths:
Build outputs under `<build>/shaders/**` (`*.spv` globally gitignored).

Origin:
Compiled by glslangValidator/DXC from tracked shader sources
(`shaders/fsr4/*.comp`, Temporal Forge-authored; `shaders/fsr1/fsr1_easu.comp`,
which additionally includes the untracked MIT-licensed FidelityFX headers
when the probe is enabled; the native INT8 packs' `passN.spv`, generated
from the AMD-copyright model HLSL, are produced by
`tools/build_native_int8_pack.sh` outside the normal build).

Classification:
Generated.

Original / upstream OSS / generated / reverse-engineered / extracted:
Generated from the sources described above.

Redistributed in repository:
No — never committed; regenerated at build time.

Upstream/source reference:
See the sections above for each generating source.

Applicable license or rights status:
Follows the generating source: build products of Temporal Forge shaders are
Apache-2.0; the opt-in FSR1 probe artifact derives from MIT SDK material;
native-pack artifacts derive from the PROVENANCE HOLD material above and
are generated locally, not distributed by this repository.

Temporal Forge licensing treatment:
No separate claim; the repository distributes sources and records the
generation methods.

Reproduction/provisioning method:
Documented in `README.md`, `resources/fsr4/native_i8/README.md`, and
`tools/build_native_int8_pack.sh`.

Notes:
None.
