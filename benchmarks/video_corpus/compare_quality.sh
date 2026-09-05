#!/usr/bin/env bash
set -euo pipefail

# Compare a player-produced clip against a trusted scaler reference.
#
# Upstream: a captured candidate and a same-source spatial reference. Downstream:
# one append-only CSV row used by RESULTS.md and the quality review harness.
# This script measures outputs; it does not modify the player or reconstruction
# configuration.
# Usage:
#   compare_quality.sh CANDIDATE REFERENCE OUTPUT_CSV [LABEL]
# The reference should be the same source/time interval and output size. Use
# make_reference below to produce deterministic Lanczos and bicubic baselines.

if (( $# < 3 || $# > 4 )); then
    printf 'usage: %s CANDIDATE REFERENCE OUTPUT_CSV [LABEL]\n' "$0" >&2
    exit 2
fi

candidate="$1"
reference="$2"
output="$3"
label="${4:-candidate}"
[[ -s "$candidate" && -s "$reference" ]] || {
    printf 'candidate and reference must both exist\n' >&2
    exit 1
}

mkdir -p "$(dirname "$output")"
if [[ ! -s "$output" ]]; then
    printf 'label,frames,psnr_mean,ssim_mean\n' > "$output"
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
ffmpeg -hide_banner -loglevel error -i "$candidate" -i "$reference" \
    -lavfi "[0:v][1:v]psnr=stats_file=$tmpdir/psnr.log" -f null -
ffmpeg -hide_banner -loglevel error -i "$candidate" -i "$reference" \
    -lavfi "[0:v][1:v]ssim=stats_file=$tmpdir/ssim.log" -f null -

frames="$(awk '/psnr_avg/ {n++} END {print n+0}' "$tmpdir/psnr.log")"
psnr="$(awk -F: '/psnr_avg/ {sum += $NF; n++} END {if (n) printf "%.6f", sum/n; else print "0"}' "$tmpdir/psnr.log")"
ssim="$(awk -F'All:' '/All:/ {split($2,a," "); if (a[1] ~ /^[0-9.]+$/) {sum += a[1]; n++}} END {if (n) printf "%.6f", sum/n; else print "0"}' "$tmpdir/ssim.log")"
printf '%s,%s,%s,%s\n' "$label" "$frames" "$psnr" "$ssim" >> "$output"

# Produce trusted spatial baselines from a source interval. This is kept as a
# callable function for corpus scripts and manual preset comparisons.
make_reference() {
    # make_reference: generate deterministic bicubic and Lanczos control clips
    # at the requested output size. run_quality.sh consumes these controls when
    # calculating deltas, so the source interval and dimensions must match the
    # candidate exactly.
    local source="$1" width="$2" height="$3" directory="$4"
    mkdir -p "$directory"
    for scaler in bicubic lanczos; do
        ffmpeg -hide_banner -loglevel error -i "$source" -an \
            -vf "scale=${width}:${height}:flags=${scaler},setsar=1" \
            -c:v ffv1 -level 3 -g 1 "${directory}/${scaler}_${width}x${height}.mkv"
    done
}
