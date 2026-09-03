# P0 recurrent-enabled qualification (production-semantic closeout: PASS)

## Current exact production-semantic qualification

The three required cases were rerun through `run_fsr_supersampling.py` with
`AMD_SEMANTIC_BASELINE`; all imported runtime-trace validations passed. The
current binary SHA is `3d2b88a27bcae3b145db75dd95ad449ce6f7d1fb244a0d75f3e475f52936c412`,
Git HEAD is `cc7daa763896d824c2caccbefc3e1d312312b2ae`, and config SHA is
`576ad1c1d6a02a95a4ef0ce732aea5440fed88d4e0277ea6c9552725d0880346`.

| Case | Source → model → output | Runtime trace | Final PNG | 2×2 score | SHA-256 |
| --- | --- | --- | --- | ---: | --- |
| cave720 | 1280×720 → 960×540 → 1920×1080 | `candidate_cave720.runtime_pipeline.json` | `candidate_cave720_fix.png` | 0.023164 | `01b2898d90479489b6875427d8cabfe34cd8d90e4eeb243415aac7c10712aa57` |
| cave360 | 640×360 → 960×540 → 1920×1080 | `candidate_cave360.runtime_pipeline.json` | `candidate_cave360_fix.png` | 0.016783 | `0f53032a1bfdbecaa738166e2877de97de225167b9d7e9db6aac90df25ea10e5` |
| cave360eq | 640×360 → 640×360 → 1280×720 | `candidate_cave360eq.runtime_pipeline.json` | `candidate_cave360eq_fix.png` | 0.030382 | `3bd9f1ecbdb705cf507b1ca544ae2400419101a6a3f7b63d3aad91a236e71388` |

All required runtime fields are present and validated: history/recurrent,
prepass input-resolve jitter, source-tap Mu-law, unjittered valid motion,
conditional reset policy, CAS off, and no geometry-mismatch reset. Independent
visual review found all three outputs clean. The following section preserves
the earlier failing qualification as historical evidence.

## Historical failed qualification

Production-path capture at frame 48 using the exact `AMD_SEMANTIC_BASELINE`
activation path, integrated best findings, CAS disabled, prepass input-resolve
jitter, source-tap Mu-law, normal decoder motion,
color history enabled, recurrent enabled, and no `TFORGE_FSR4_FORCE_RESET`.
The committed temporal fix (`u_historyOut = upscaledColor`) is unchanged.

| Case | Source → model → output | Final PNG | 2×2 score | SHA-256 |
| --- | --- | --- | ---: | --- |
| cave720 | 1280×720 → 960×540 → 1920×1080 | `cave720.png` | 0.054748 | `616c7d269945d3211db28392cf62b5910a52b5c369e3c14e3b7b0fdaf8ad8e73` |
| cave360 | 640×360 → 960×540 → 1920×1080 | `cave360.png` | 0.028079 | `e111f66fe82f4aaba5c8f5622092ecacccbc1424246e380f17e867cad87f53c2` |
| cave360eq | 640×360 → 640×360 → 1280×720 | `cave360eq.png` | 0.030384 | `27d1b46e5502e85d33298eff035833fbf88a7ac666b146b050b04b262eb447e4` |

The runtime traces pass the exact AMD semantic validator, but the two mismatch
outputs fail visual lattice review. The adjacent `*.runtime_trace.json` files are authoritative
runtime provenance: each reports `history_enabled: true`,
`recurrent_enabled: true`, `cas_enabled: false`, prepass input-resolve jitter,
source-tap Mu-law, normal
motion semantics, and the normal conditional reset policy
(`seek_resize_cut_or_invalid_correspondence`), with no forced-reset override.

Capture provenance: Git HEAD `5784c9e0690d49577dc6e8bf33ee8870a6830623`;
binary SHA-256
`f0756ef7dbc86b18fb0c1afb5393552862260f374c5f8dc5ad375ac150850983`;
`benchmark_settings.json` SHA-256
`576ad1c1d6a02a95a4ef0ce732aea5440fed88d4e0277ea6c9552725d0880346`.

Human visual review: FAIL for `cave720` and `cave360` (visible repeating
lattice); PASS for `cave360eq`. No unrelated trails, stale contamination,
halos, or color shifts were observed.

Controlled semantic A/B evidence is retained outside this directory: disabling
source-tap Mu-law reduced the 720→1080 score from 0.054748 to 0.020051,
while changing jitter mode alone left it at 0.055246. This isolates the
remaining defect to the source-tap Mu-law path under source/model mismatch.
