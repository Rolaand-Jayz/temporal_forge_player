# Temporal Forge review harness

Open `index.html` directly in a browser. The harness has no build step, server,
framework, or network dependency. It is responsive, but a wide desktop view is
best for pixel inspection.

The active matrix contains only 11 upscale routes: 360p to 480p/720p/1080p,
480p to 720p/1080p/1440p, 720p to 1080p/1440p/2160p, and 1080p to
1440p/2160p. Missing validated assets display `images/no_image.svg`; arbitrary
input/output combinations are not selectable.

`benchmarks/quality_sweeps/run_quality_campaign_capture.py` publishes the
validated campaign images and `catalog.js` together after each complete pair.
The harness does not have a separate capture pass. Supersampling labels are
delivery-grid multipliers; recorded reconstruction dimensions remain the
authority when reviewing a completed row.
