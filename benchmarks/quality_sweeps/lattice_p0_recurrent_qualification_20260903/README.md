# P0 recurrent-enabled qualification

Production-path capture at frame 48 using `AMD_SEMANTIC_BASELINE`, integrated
best findings, CAS disabled, synthetic Halton jitter, normal decoder motion,
color history enabled, recurrent enabled, and no `TFORGE_FSR4_FORCE_RESET`.
The committed temporal fix (`u_historyOut = upscaledColor`) is unchanged.

| Case | Source → model → output | Final PNG | 2×2 score | SHA-256 |
| --- | --- | --- | ---: | --- |
| cave720 | 1280×720 → 960×540 → 1920×1080 | `cave720.png` | 0.023211 | `60a99afb2af7ba5272f47da3a6a251d4995d2d1a537fb3446aa3660652f768c9` |
| cave360 | 640×360 → 960×540 → 1920×1080 | `cave360.png` | 0.016872 | `4a2e2eb73a48cfd2de6a79b7f3cff236a245248e3755d10d7039c17331373c66` |
| cave360eq | 640×360 → 640×360 → 1280×720 | `cave360eq.png` | 0.030349 | `96151024ae14f01d3b0a929bc96d381a575d52e2fc3a616b3faae9c7eb08ae42` |

All scores are below the 0.20 fail-closed threshold and remain in the clean
post-fix range. The adjacent `*.runtime_trace.json` files are authoritative
runtime provenance: each reports `history_enabled: true`,
`recurrent_enabled: true`, `cas_enabled: false`, synthetic jitter, normal
motion semantics, and the normal conditional reset policy
(`seek_resize_cut_or_invalid_correspondence`), with no forced-reset override.

Capture provenance: Git HEAD `b588d20a530942097b1e86444b97864ff42b36dd`;
binary SHA-256
`f0756ef7dbc86b18fb0c1afb5393552862260f374c5f8dc5ad375ac150850983`;
`benchmark_settings.json` SHA-256
`576ad1c1d6a02a95a4ef0ce732aea5440fed88d4e0277ea6c9552725d0880346`.

Human visual review: PASS by independent capture worker. All three frame-48
outputs and enlarged crops were clean: no lattice/checkerboard, recurrent
trails, stale contamination, halos, color shifts, or instability. The 360→1080
case is naturally softer from the severe upscale; the source==model control has
appropriately stronger detail.
