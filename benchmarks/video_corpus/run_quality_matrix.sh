#!/usr/bin/env bash
set -euo pipefail

# Run the same corpus selection through every user-facing preset. Each preset
# gets an independent CSV so a failed run cannot be mistaken for a complete
# matrix. run_quality.sh supplies the trusted reference and Lanczos baseline.
#
# Usage:
#   run_quality_matrix.sh PLAYER SELECTOR OUTPUT_DIRECTORY

if (( $# != 3 )); then
    printf 'usage: %s PLAYER SELECTOR OUTPUT_DIRECTORY\n' "$0" >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
player="$(realpath "$1")"
selector="$2"
output_dir="$3"

[[ -x "$player" ]] || {
    printf 'player binary is not executable: %s\n' "$player" >&2
    exit 1
}

mkdir -p "$output_dir"
for preset in Off NativeAA Quality Balanced Performance UltraPerformance; do
    safe_name="${preset,,}"
    result_csv="$output_dir/${safe_name}.csv"
    TFORGE_BENCHMARK_PRESET="$preset" \
    TFORGE_QUALITY_TAG="matrix_${safe_name}" \
    "$root/run_quality.sh" "$player" "$selector" \
        "$result_csv"
    if [[ ! -s "$result_csv" ]] || ! tail -n +2 "$result_csv" | grep -q '[^[:space:]]'; then
        printf 'preset %s produced no quality rows; matrix is incomplete (%s)\n' \
            "$preset" "$result_csv" >&2
        exit 1
    fi
done

printf 'quality matrix complete: %s\n' "$output_dir"
