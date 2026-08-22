#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
    printf 'usage: %s INPUT_IMAGE OUTPUT_IMAGE\n' "$0" >&2
    exit 2
fi

input="$(realpath "$1")"
output="$2"
[[ -s "$input" ]] || { printf 'input does not exist: %s\n' "$input" >&2; exit 1; }
command -v ffmpeg >/dev/null || { printf 'ffmpeg is required\n' >&2; exit 1; }

read -r width height < <(sed -n '2p' "$input")
[[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ ]] || {
    printf 'input must expose width and height on its second line: %s\n' "$input" >&2
    exit 1
}

mkdir -p "$(dirname "$output")"

config="${TFORGE_REVIEW_PIPELINE_CONFIG:-$(dirname "$0")/review_pipeline.env}"
if [[ -f "$config" ]]; then
    # shellcheck disable=SC1090
    source "$config"
fi
final_scale="${TFORGE_REVIEW_FINAL_SCALE:-1}"
final_filter="${TFORGE_REVIEW_FINAL_FILTER:-bicubic}"
final_cas="${TFORGE_REVIEW_FINAL_CAS:-0}"
filters=()
if [[ "$final_scale" == 1 ]]; then filters+=("scale=${width}:${height}:flags=${final_filter}"); fi
if [[ "$final_cas" != 0 ]]; then filters+=("cas=${final_cas}"); fi
filtergraph="$(IFS=,; echo "${filters[*]}")"
ffmpeg -hide_banner -loglevel error -y \
    -i "$input" \
    -vf "$filtergraph" \
    -frames:v 1 "$output"
