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
ffmpeg -hide_banner -loglevel error -y \
    -i "$input" \
    -vf "scale=${width}:${height}:flags=lanczos" \
    -frames:v 1 "$output"
