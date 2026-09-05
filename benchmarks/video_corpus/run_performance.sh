#!/usr/bin/env bash
set -euo pipefail

# run_performance.sh — measure player timing over manifest-selected clips.
#
# Upstream: prepare_corpus.sh's manifest and a build-fast player. Downstream:
# performance.csv plus per-clip logs used to compare frame-time behavior. It
# intentionally reports timings and does not alter quality settings or images.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
binary="${1:-$repo/build-fast/temporal_forge_player}"
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
    'clip_id,width,height,quality,crf,input_mbps,frames,decode_mean_ms,decode_p50_ms,decode_p95_ms,upload_mean_ms,upload_p50_ms,upload_p95_ms,presentation_mean_ms,presentation_p50_ms,presentation_p95_ms,pipeline_mean_ms,pipeline_p50_ms,pipeline_p95_ms,dispatch_mean_ms,record_mean_ms,wait_gpu_mean_ms,gpu_mean_ms,gpu_p50_ms,gpu_p95_ms' \
    > "$results"

summary() {
    # summary: reduce sampled stage/pipeline/dispatch/GPU timings to count, mean,
    # median, and p95. The caller writes these aggregates beside the clip's
    # metadata so later reviews can distinguish latency from image quality.
    local samples="$1"
    local column="$2"
    local sorted
    sorted="$(mktemp)"
    # The input is already a filtered numeric sample file produced by sed
    # below; it has no log header to skip. Filtering by numeric field keeps
    # malformed lines out without silently discarding valid short runs.
    awk -v column="$column" 'NF >= column && $column ~ /^[0-9]+([.][0-9]+)?$/ { print $column }' "$samples" | sort -n > "$sorted"
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
    # Measure normal playback by default. The synchronous mode is useful for
    # capture/debug comparisons, but it waits for every Vulkan dispatch and
    # therefore reports capture-path CPU stalls rather than steady-state
    # playback cost. Keep it available as an explicit diagnostic toggle.
    performance_environment=(
        TFORGE_HEADLESS_BENCHMARK=1
        TFORGE_FSR4_LOG_INTERVAL=1
        TFORGE_FSR4_PROFILE_TIMINGS=1
    )
    if [[ -n "${TFORGE_PERFORMANCE_SYNCHRONOUS:-}" ]]; then
        performance_environment+=(TFORGE_FSR4_DISABLE_INFLIGHT=1)
    fi
    set +e
    timeout 12s env "${performance_environment[@]}" \
        "$binary" "$path" > "$log" 2>&1
    status=$?
    set -e
    if (( status != 0 && status != 124 )); then
        printf 'Player failed for %s (status %s); see %s\n' "$path" "$status" "$log" >&2
        continue
    fi

    sed -n \
        's/.*stage-timing decodeCPU=\([0-9.]*\)ms uploadCPU=\([0-9.]*\)ms presentationCPU=\([0-9.]*\)ms pipelineCPU=\([0-9.]*\)ms dispatchCPU=\([0-9.]*\)ms recordCPU=\([0-9.]*\)ms waitGPUCPU=\([0-9.]*\)ms GPU=\([0-9.]*\)ms.*/\1 \2 \3 \4 \5 \6 \7 \8/p' \
        "$log" > "$samples"
    decode="$(summary "$samples" 1)"
    upload="$(summary "$samples" 2)"
    presentation="$(summary "$samples" 3)"
    pipeline="$(summary "$samples" 4)"
    dispatch="$(summary "$samples" 5)"
    record="$(summary "$samples" 6)"
    wait_gpu="$(summary "$samples" 7)"
    gpu="$(summary "$samples" 8)"
    IFS=, read -r _decode_count decode_mean decode_p50 decode_p95 <<< "$decode"
    IFS=, read -r _upload_count upload_mean upload_p50 upload_p95 <<< "$upload"
    IFS=, read -r _presentation_count presentation_mean presentation_p50 presentation_p95 <<< "$presentation"
    IFS=, read -r frames pipeline_mean pipeline_p50 pipeline_p95 <<< "$pipeline"
    IFS=, read -r _dispatch_count dispatch_mean _dispatch_p50 _dispatch_p95 <<< "$dispatch"
    IFS=, read -r _record_count record_mean _record_p50 _record_p95 <<< "$record"
    IFS=, read -r _wait_gpu_count wait_gpu_mean _wait_gpu_p50 _wait_gpu_p95 <<< "$wait_gpu"
    IFS=, read -r _gpu_count gpu_mean gpu_p50 gpu_p95 <<< "$gpu"
    bitrate="$(ffprobe -v error -show_entries format=bit_rate -of csv=p=0 "$path")"
    input_mbps="$(awk -v bits="${bitrate:-0}" 'BEGIN { printf "%.3f", bits / 1000000.0 }')"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$clip_id" "$width" "$height" "$quality" "$crf" "$input_mbps" \
        "$frames" "$decode_mean" "$decode_p50" "$decode_p95" \
        "$upload_mean" "$upload_p50" "$upload_p95" \
        "$presentation_mean" "$presentation_p50" "$presentation_p95" \
        "$pipeline_mean" "$pipeline_p50" "$pipeline_p95" \
        "$dispatch_mean" "$record_mean" "$wait_gpu_mean" \
        "$gpu_mean" "$gpu_p50" "$gpu_p95" >> "$results"
done
exec 3<&-

printf 'Performance results: %s\n' "$results"
