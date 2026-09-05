#!/usr/bin/env bash
set -euo pipefail

# Run multi-frame temporal quality for every user-facing FSR preset.  Off is
# intentionally excluded: it bypasses the FSR output and is covered by the
# single-frame quality matrix as the raw/reference baseline.
# Usage: run_temporal_quality_matrix.sh PLAYER INPUT REFERENCE OUTPUT_DIR [FRAMES]
#        [--retries N] [--workers N] [--dry-run]

if (( $# < 4 )); then
    printf 'usage: %s PLAYER INPUT REFERENCE OUTPUT_DIR [FRAMES] [--retries N] [--workers N] [--dry-run]\n' "$0" >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
player="$(realpath "$1")"
input="$(realpath "$2")"
reference="$(realpath "$3")"
output_dir="$4"
frames=8
retries=1
# A single Vulkan device and one CPU-heavy decoder per capture make an
# unbounded process fan-out slower. Two workers is the measured starting point
# on the development machine; callers can override it for another machine.
workers="${TFORGE_CAPTURE_WORKERS:-2}"
dry_run=0
shift 4
if (( $# > 0 )) && [[ "$1" != --* ]]; then
    frames="$1"
    shift
fi
while (( $# > 0 )); do
    case "$1" in
        --retries)
            [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]] || {
                printf '%s requires a non-negative integer\n' "$1" >&2
                exit 2
            }
            retries="$2"
            shift 2
            ;;
        --workers)
            [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || {
                printf '%s requires a positive integer\n' "$1" >&2
                exit 2
            }
            workers="$2"
            shift 2
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        *)
            printf 'unknown option: %s\n' "$1" >&2
            exit 2
            ;;
    esac
done
[[ "$frames" =~ ^[1-9][0-9]*$ ]] || {
    printf 'frames must be a positive integer\n' >&2
    exit 2
}
[[ "$workers" =~ ^[1-9][0-9]*$ ]] || {
    printf 'workers must be a positive integer\n' >&2
    exit 2
}
[[ -x "$player" && -s "$input" && -s "$reference" ]] || {
    printf 'player, input, and reference must exist\n' >&2
    exit 1
}
if (( ! dry_run )); then
    mkdir -p "$output_dir"
fi

run_one() {
    local preset="$1"
    local safe_name="$2"
    local attempt="$3"
    local attempt_dir="$output_dir/$safe_name/attempt-$attempt"
    local result="$attempt_dir/quality.csv"
    local failure_dir="$attempt_dir/failure-artifacts"

    mkdir -p "$attempt_dir"
    # Independent captures already occupy the CPU with decode and metric
    # work. Keep each FFmpeg child to one worker by default so the requested
    # capture fan-out does not turn into thread-pool contention. Callers can
    # override this when they deliberately want threaded FFmpeg.
    TFORGE_BENCHMARK_FFMPEG_THREADS="${TFORGE_BENCHMARK_FFMPEG_THREADS:-1}" \
    TFORGE_BENCHMARK_PRESET="$preset" \
        TFORGE_QUALITY_TAG="matrix_${safe_name}_attempt_${attempt}" \
        TFORGE_TEMPORAL_ARTIFACT_DIR="$attempt_dir/artifacts" \
        TFORGE_TEMPORAL_FAILURE_ARTIFACT_DIR="$failure_dir" \
        "$root/run_temporal_quality.sh" "$player" "$input" "$reference" \
        "$result" "$frames"

    [[ -s "$result" ]] && tail -n +2 "$result" | grep -q '[^[:space:]]'
}

run_preset() {
    # Each preset owns its attempt directory and final CSV. This lets worker
    # processes overlap without sharing generated images or partial results.
    local preset="$1"
    local safe_name="${preset,,}"
    local final_result="$output_dir/${safe_name}.csv"
    local success=0

    for ((attempt = 0; attempt <= retries; ++attempt)); do
        if run_one "$preset" "$safe_name" "$attempt"; then
            cp "$output_dir/$safe_name/attempt-$attempt/quality.csv" "$final_result"
            printf 'preset %s succeeded on attempt %d: %s\n' \
                "$preset" "$attempt" "$final_result"
            success=1
            break
        fi
        printf 'preset %s attempt %d failed; retained artifacts under %s\n' \
            "$preset" "$attempt" "$output_dir/$safe_name/attempt-$attempt" >&2
    done
    if (( ! success )); then
        printf 'preset %s failed after %d attempt(s)\n' \
            "$preset" "$((retries + 1))" >&2
        return 1
    fi
}

presets=(NativeAA Quality Balanced Performance UltraPerformance)
if (( dry_run )); then
    printf 'capture workers=%s\n' "$workers"
    for preset in "${presets[@]}"; do
        safe_name="${preset,,}"
        printf 'preset=%s attempts=%s..%s result=%s\n' \
            "$preset" 0 "$retries" "$output_dir/${safe_name}.csv"
    done
    printf 'temporal quality matrix dry run: %s\n' "$output_dir"
    exit 0
fi

# Keep a bounded pool instead of launching all presets at once. The capture
# itself is GPU work, while decode, frame conversion, and metric extraction
# consume CPU; the bound prevents those stages from starving one another.
active_pids=()
matrix_failed=0
for preset in "${presets[@]}"; do
    run_preset "$preset" &
    active_pids+=("$!")
    if (( ${#active_pids[@]} >= workers )); then
        if ! wait "${active_pids[0]}"; then
            matrix_failed=1
        fi
        active_pids=("${active_pids[@]:1}")
    fi
done
while (( ${#active_pids[@]} > 0 )); do
    if ! wait "${active_pids[0]}"; then
        matrix_failed=1
    fi
    active_pids=("${active_pids[@]:1}")
done

if (( matrix_failed )); then
    printf 'temporal quality matrix failed; inspect per-preset attempt artifacts under %s\n' "$output_dir" >&2
    exit 1
fi
printf 'temporal quality matrix complete: %s\n' "$output_dir"
