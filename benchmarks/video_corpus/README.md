# Real-world video corpus

This corpus replaces generated test patterns for FSR4 video performance and
quality work. It uses official Blender Open Movie masters and controlled local
encodes so resolution and compression can be varied independently for the same
frames.

## License and attribution

The movies are open content under Creative Commons Attribution 3.0. They are
free to download, test, modify, and redistribute with attribution. Keep this
attribution with derived test clips:

- Tears of Steel, copyright Blender Foundation, mango.blender.org
- Sintel, copyright Blender Foundation, durian.blender.org
- Big Buck Bunny, copyright Blender Foundation, peach.blender.org

Official source and license pages:

- https://mango.blender.org/about/
- https://durian.blender.org/about/
- https://durian.blender.org/sharing/
- https://download.blender.org/demo/movies/ToS/
- https://download.blender.org/durian/movies/
- https://download.blender.org/peach/bigbuckbunny_movies/

## Coverage

Six ten-second scenes cover live action, faces, architecture, hard silhouettes,
VFX debris, grass, foliage, fur, fast motion, smoke, dark gradients, and fine
crystal texture. Every scene has this input matrix:

| Dimension | Values |
|---|---|
| Resolution | 426x240, 640x360, 854x480, 1280x720, 1920x1080 |
| Compression | high CRF 12, medium CRF 23, low CRF 35 |
| Codec | H.264, 8-bit 4:2:0, fixed 48-frame GOP |

The player conversion path uses decoded range and matrix metadata. The corpus
is BT.709 limited, while separate functional validation covers full-range
BT.601 using a real corpus scene transcode.

Tears of Steel and Sintel inputs are derived from official 4K masters. Their
lossless 3840x2160 reference clips permit full-reference image metrics and
edge-artifact comparisons. Big Buck Bunny adds animation and foliage diversity,
but its local source is 1080p and therefore is not used for 4K reference scores.

## Preparation

```bash
./benchmarks/video_corpus/prepare_corpus.sh
```

Downloads are resumable and existing files are reused. Generated files are
indexed in `manifest.csv` and checksummed in `CORPUS_SHA256SUMS`.

Do not average the entire corpus into one unexplained score. Report latency and
quality by scene, resolution, and compression level. Checkerboard or edge-blend
failures can otherwise disappear behind easy static scenes.

## Measurement

Run the complete 720p performance matrix:

```bash
./benchmarks/video_corpus/run_performance.sh \
  ./build/temporal_forge_player '1280x720' \
  benchmarks/video_corpus/results/performance_720p.csv
```

Run full-reference quality measurements for the scenes with native 4K masters:

```bash
TFORGE_QUALITY_FRAME=48 ./benchmarks/video_corpus/run_quality.sh \
  ./build/temporal_forge_player '1280x720' \
  benchmarks/video_corpus/results/quality_720p.csv
```

The maintained low-resolution scope is 240p, 360p, and 480p. Treat 240p as
the floor and run it independently so its severe reconstruction ratio is not
hidden by higher-resolution averages:

```bash
XDG_CONFIG_HOME=/tmp/tforge-ultra-config \
  ./benchmarks/video_corpus/run_performance.sh \
  ./build/temporal_forge_player '426x240' \
  benchmarks/video_corpus/results/performance_240p_full.csv
```

```bash
TFORGE_QUALITY_FRAME=48 ./benchmarks/video_corpus/run_quality.sh \
  ./build/temporal_forge_player '426x240' \
  benchmarks/video_corpus/results/quality_240p.csv
```

Use the selector `426x240|640x360|854x480` when one run should cover the
complete sub-720p matrix. Keep per-resolution result files for comparisons;
do not collapse these tiers into one aggregate score.

The quality runner reports FSR PSNR, SSIM, edge SSIM, a Lanczos control, and
per-metric deltas at the player's actual output dimensions. It also preserves
output and amplified difference images under `results/quality_frames`. Set
`TFORGE_QUALITY_TAG` to keep side-by-side sweep artifacts without overwriting
earlier frames. Current measured results and limitations are in `RESULTS.md`.
