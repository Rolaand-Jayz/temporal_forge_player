#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
    printf 'usage: %s INPUT_VIDEO OUTPUT_VIDEO\n' "$0" >&2
    exit 2
fi

input="$(realpath "$1")"
output="$2"
[[ -s "$input" ]] || { printf 'input does not exist: %s\n' "$input" >&2; exit 1; }
command -v ffmpeg >/dev/null || { printf 'ffmpeg is required\n' >&2; exit 1; }

mkdir -p "$(dirname "$output")"

config="${TFORGE_REVIEW_PIPELINE_CONFIG:-$(dirname "$0")/review_pipeline.env}"
if [[ -f "$config" ]]; then
    # shellcheck disable=SC1090
    source "$config"
fi
pre_scale="${TFORGE_REVIEW_PRE_SCALE:-1}"
pre_filter="${TFORGE_REVIEW_PRE_FILTER:-bicubic}"
pre_denoise="${TFORGE_REVIEW_PRE_DENOISE:-0}"
pre_denoise_filter="${TFORGE_REVIEW_PRE_DENOISE_FILTER:-hqdn3d=1.0:1.0:3.0:3.0}"
pre_cas="${TFORGE_REVIEW_PRE_CAS:-0}"
filters=()
if [[ "$pre_scale" == 1 ]]; then filters+=("scale=iw:ih:flags=${pre_filter}"); fi
if [[ "$pre_denoise" == 1 ]]; then filters+=("${pre_denoise_filter}"); fi
if [[ "$pre_cas" != 0 ]]; then filters+=("cas=${pre_cas}"); fi
filters+=(setsar=1)
filtergraph="$(IFS=,; echo "${filters[*]}")"

# Apply exactly one bicubic pass at the decoded native dimensions. There is
# deliberately no intermediate upscale or return downscale before FSR.
ffmpeg -hide_banner -loglevel error -y \
    -i "$input" \
    -vf "$filtergraph" \
    -an -c:v ffv1 -level 3 -g 1 "$output"
