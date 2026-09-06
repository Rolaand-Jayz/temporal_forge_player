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

# Apply Lanczos at the decoded native dimensions. There is deliberately no
# intermediate upscale and no return downscale: the resulting video remains
# the same width and height as the input before it is sent through FSR.
ffmpeg -hide_banner -loglevel error -y \
    -i "$input" \
    -vf 'scale=iw:ih:flags=lanczos,setsar=1' \
    -an -c:v ffv1 -level 3 -g 1 "$output"
