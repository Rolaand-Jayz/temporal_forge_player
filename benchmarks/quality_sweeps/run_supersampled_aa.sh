#!/usr/bin/env bash
set -euo pipefail

# Build the authoritative 4x vector anti-aliasing ground truth and run the
# current Quality Lab reconstruction against the matching low-resolution
# source. The generated corpus is deliberately outside the repository because
# the 7680x4320 master is a benchmark artifact, not a runtime asset.
#
# Usage: run_supersampled_aa.sh [PLAYER] [OUTPUT_ROOT]

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
player="${1:-$repo/build-fast/temporal_forge_player}"
output="${2:-/tmp/tforge-supersampled-aa-20260821}"
[[ -x "$player" ]] || {
    printf 'Player binary is not executable: %s\n' "$player" >&2
    exit 1
}
[[ ! -e "$output" ]] || {
    printf 'Refusing to overwrite existing output root: %s\n' "$output" >&2
    exit 1
}

mkdir -p "$output"
master="$output/master_7680x4320.png"
reference="$output/reference_1920x1080.mkv"
low_source="$output/source_426x240.mkv"
reference_still="$output/reference_1920x1080_f48.png"
manifest="$output/manifest.csv"
sweep_manifest="$output/sweep_manifest.json"

rsvg-convert -w 7680 -h 4320 "$root/supersampled_aa/master.svg" -o "$master"

ffmpeg -hide_banner -loglevel error -y -loop 1 -i "$master" \
    -vf 'scale=1920:1080:flags=lanczos,setsar=1' -t 2 -r 30 -an \
    -c:v ffv1 -level 3 -g 1 -pix_fmt yuv444p "$reference"
ffmpeg -hide_banner -loglevel error -y -loop 1 -i "$master" \
    -vf 'scale=426:240:flags=lanczos,setsar=1' -t 2 -r 30 -an \
    -c:v ffv1 -level 3 -g 1 -pix_fmt yuv420p "$low_source"
ffmpeg -hide_banner -loglevel error -y -i "$reference" \
    -vf 'select=eq(n\,48)' -frames:v 1 "$reference_still"

printf '%s\n' \
    'clip_id,title,source_url,license,start_seconds,duration_seconds,width,height,quality,crf,path,reference_path' \
    > "$manifest"
printf '%s\n' \
    "supersampled_aa,4x vector anti-aliasing ground truth,synthetic,CC0,0,2,426,240,high,0,$low_source,$reference" \
    >> "$manifest"

cat > "$sweep_manifest" <<EOF
{
  "name": "supersampled-aa-ground-truth",
  "corpusManifest": "$manifest",
  "preset": "Quality",
  "dimensions": "426x240",
  "frame": 48,
  "quality": "high",
  "clipRegex": "^supersampled_aa$",
  "experiments": [
    {
      "id": "stable_base_tone_m015",
      "baseConfig": "$repo/benchmarks/quality_sweeps/stage_e/tone_template.json",
      "overrides": {
        "qualityLab": {
          "tone": { "exposureEV": -0.015 }
        }
      }
    }
  ]
}
EOF

TFORGE_QUALITY_MANIFEST="$manifest" \
    python3 "$root/run_quality_sweep.py" \
    --manifest "$sweep_manifest" \
    --binary "$player" \
    --output-root "$output/sweep" \
    --tag-prefix supersampled-aa

printf 'supersampled anti-aliasing artifacts: %s\n' "$output"
