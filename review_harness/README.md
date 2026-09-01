# Temporal Forge review harness

Open `index.html` directly in a desktop browser. The harness has no build step,
server, framework, or network dependency. Add result PNGs to `images/` using
the canonical filename shown in the technical readout; refresh the page and
select the matching scene, frame, input, method, and output values.

Missing combinations remain selectable and display `images/no_image.svg`.
Export campaign results with `tools/export_review_image.py` so names are
validated and constructed consistently. Supersampling labels are
delivery-grid multipliers: the runner computes intermediate size as
`final_size × multiplier ÷ 2`; recorded intermediate dimensions are
authoritative when comparing resolutions.
