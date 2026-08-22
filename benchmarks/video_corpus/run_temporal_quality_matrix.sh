#!/usr/bin/env bash
set -euo pipefail

# Run multi-frame temporal quality for every user-facing FSR preset.  Off is
# intentionally excluded: it bypasses the FSR output and is covered by the
# single-frame quality matrix as the raw/reference baseline.
# Usage: run_temporal_quality_matrix.sh PLAYER INPUT REFERENCE OUTPUT_DIR [FRAMES]

if (( $# < 4 || $# > 5 )); then
    printf 'usage: %s PLAYER INPUT REFERENCE OUTPUT_DIR [FRAMES]\n' "$0" >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
player="$(realpath "$1")"
input="$(realpath "$2")"
reference="$(realpath "$3")"
output_dir="$4"
frames="${5:-8}"
[[ -x "$player" && -s "$input" && -s "$reference" ]] || {
    printf 'player, input, and reference must exist\n' >&2
    exit 1
}
mkdir -p "$output_dir"

for preset in NativeAA Quality Balanced Performance UltraPerformance; do
    safe_name="${preset,,}"
    result="$output_dir/${safe_name}.csv"
    TFORGE_BENCHMARK_PRESET="$preset" \
        "$root/run_temporal_quality.sh" "$player" "$input" "$reference" \
        "$result" "$frames"
    if [[ ! -s "$result" ]] || ! tail -n +2 "$result" | grep -q '[^[:space:]]'; then
        printf 'preset %s produced no temporal quality row\n' "$preset" >&2
        exit 1
    fi
done
printf 'temporal quality matrix complete: %s\n' "$output_dir"
