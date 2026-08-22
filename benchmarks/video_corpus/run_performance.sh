#!/usr/bin/env bash
set -euo pipefail

# run_performance.sh — measure player timing over manifest-selected clips.
#
# Upstream: prepare_corpus.sh's manifest and a build-fast player. Downstream:
# performance.csv plus per-clip logs used to compare frame-time behavior. It
# intentionally reports timings and does not alter quality settings or images.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
binary="${1:-$repo/build/temporal_forge_player}"
selector="${2:-.*}"
results="${3:-$root/results/performance.csv}"
logs="$root/results/logs"

if [[ ! -x "$binary" ]]; then
    printf 'Player binary is not executable: %s\n' "$binary" >&2
    exit 1
fi
if [[ ! -s "$root/manifest.csv" ]]; then
    printf 'Prepare the corpus first: %s/prepare_corpus.sh\n' "$root" >&2
    exit 1
fi

mkdir -p "$logs" "$(dirname "$results")"
printf '%s\n' \
    'clip_id,width,height,quality,crf,input_mbps,frames,pipeline_mean_ms,pipeline_p50_ms,pipeline_p95_ms,dispatch_mean_ms,gpu_mean_ms,gpu_p50_ms,gpu_p95_ms' \
    > "$results"

summary() {
    # summary: reduce sampled pipeline/dispatch/GPU timings to count, mean,
    # median, and p95. The caller writes these aggregates beside the clip's
    # metadata so later reviews can distinguish latency from image quality.
    local samples="$1"
    local column="$2"
    local sorted
    sorted="$(mktemp)"
    awk -v column="$column" 'NR > 12 { print $column }' "$samples" | sort -n > "$sorted"
    local count
    count="$(wc -l < "$sorted")"
    if (( count == 0 )); then
        rm -f "$sorted"
        printf '0,0,0,0'
        return
    fi
    local mean p50 p95
    mean="$(awk '{ sum += $1 } END { printf "%.4f", sum / NR }' "$sorted")"
    p50="$(sed -n "$(( (count + 1) / 2 ))p" "$sorted")"
    p95="$(sed -n "$(( (95 * count + 99) / 100 ))p" "$sorted")"
    rm -f "$sorted"
    printf '%s,%s,%s,%s' "$count" "$mean" "$p50" "$p95"
}

exec 3< "$root/manifest.csv"
IFS= read -r _header <&3
while IFS=, read -r \
    clip_id title source_url license start duration width height quality crf path reference <&3; do
    if [[ ! "$path" =~ $selector ]]; then
        continue
    fi

    log="$logs/$(basename "${path%.*}").log"
    samples="$logs/$(basename "${path%.*}").samples"
    printf 'Benchmarking %s %sx%s %s...\n' "$clip_id" "$width" "$height" "$quality"
    set +e
    timeout 12s env \
        TFORGE_HEADLESS_BENCHMARK=1 \
        TFORGE_FSR4_LOG_INTERVAL=1 \
        "$binary" "$path" > "$log" 2>&1
    status=$?
    set -e
    if (( status != 0 && status != 124 )); then
        printf 'Player failed for %s (status %s); see %s\n' "$path" "$status" "$log" >&2
        continue
    fi

    sed -n \
        's/.*pipelineCPU=\([0-9.]*\)ms dispatchCPU[^=]*=\([0-9.]*\)ms GPU[^=]*=\([0-9.]*\)ms.*/\1 \2 \3/p' \
        "$log" > "$samples"
    pipeline="$(summary "$samples" 1)"
    dispatch="$(summary "$samples" 2)"
    gpu="$(summary "$samples" 3)"
    frames="${pipeline%%,*}"
    pipeline_values="${pipeline#*,}"
    dispatch_mean="$(cut -d, -f2 <<< "$dispatch")"
    gpu_values="$(cut -d, -f2-4 <<< "$gpu")"
    bitrate="$(ffprobe -v error -show_entries format=bit_rate -of csv=p=0 "$path")"
    input_mbps="$(awk -v bits="${bitrate:-0}" 'BEGIN { printf "%.3f", bits / 1000000.0 }')"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$clip_id" "$width" "$height" "$quality" "$crf" "$input_mbps" \
        "$frames" "$pipeline_values" "$dispatch_mean" "$gpu_values" >> "$results"
done
exec 3<&-

printf 'Performance results: %s\n' "$results"
