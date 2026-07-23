#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
binary="${1:-$repo/build/temporal_forge_player}"
selector="${2:-1280x720}"
results="${3:-$root/results/quality.csv}"
frame_index="${TFORGE_QUALITY_FRAME:-48}"
tag="${TFORGE_QUALITY_TAG:-}"
if [[ -n "$tag" && ! "$tag" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    printf 'TFORGE_QUALITY_TAG contains unsupported filename characters: %s\n' "$tag" >&2
    exit 1
fi
tag_suffix="${tag:+_$tag}"
frames="$root/results/quality_frames"
logs="$root/results/quality_logs"

if [[ ! -x "$binary" ]]; then
    printf 'Player binary is not executable: %s\n' "$binary" >&2
    exit 1
fi

mkdir -p "$frames" "$logs" "$(dirname "$results")"
printf '%s\n' \
    'clip_id,width,height,output_width,output_height,scale,quality,crf,frame,fsr_psnr_db,fsr_ssim,fsr_edge_ssim,lanczos_psnr_db,lanczos_ssim,lanczos_edge_ssim,ssim_delta,edge_ssim_delta,output_path,difference_path' \
    > "$results"

metric_value() {
    local label="$1"
    local file="$2"
    grep -o "${label}:[-0-9.]*" "$file" | tail -1 | cut -d: -f2
}

exec 3< "$root/manifest.csv"
IFS= read -r _header <&3
while IFS=, read -r \
    clip_id title source_url license start duration width height quality crf path reference <&3; do
    if [[ -z "$reference" || ! "$path" =~ $selector ]]; then
        continue
    fi

    stem="$(basename "${path%.*}")"
    output_ppm="$frames/${stem}_f${frame_index}${tag_suffix}.ppm"
    output_png="$frames/${stem}_f${frame_index}${tag_suffix}.png"
    lanczos_png="$frames/${stem}_f${frame_index}${tag_suffix}_lanczos.png"
    difference_png="$frames/${stem}_f${frame_index}${tag_suffix}_difference.png"
    log="$logs/${stem}${tag_suffix}.log"

    printf 'Capturing %s %sx%s %s frame %s...\n' \
        "$clip_id" "$width" "$height" "$quality" "$frame_index"
    rm -f "$output_ppm" "$output_png" "$lanczos_png" "$difference_png"
    set +e
    env \
        TFORGE_HEADLESS_BENCHMARK=1 \
        TFORGE_FSR4_DUMP_OUTPUT=1 \
        TFORGE_FSR4_DUMP_OUTPUT_FRAME="$frame_index" \
        TFORGE_FSR4_DUMP_OUTPUT_PATH="$output_ppm" \
        "$binary" "$path" < /dev/null > "$log" 2>&1 &
    player_pid=$!
    status=124
    output_width=0
    output_height=0
    expected_output_bytes=0
    for ((attempt = 0; attempt < 120; ++attempt)); do
        output_bytes=0
        if [[ -f "$output_ppm" ]]; then
            output_bytes="$(stat -c %s "$output_ppm")"
            dimensions="$(sed -n '2p' "$output_ppm" 2>/dev/null || true)"
            if [[ "$dimensions" =~ ^([0-9]+)[[:space:]]+([0-9]+)$ ]]; then
                output_width="${BASH_REMATCH[1]}"
                output_height="${BASH_REMATCH[2]}"
                header_bytes="$(printf 'P6\n%s\n255\n' "$dimensions" | wc -c)"
                expected_output_bytes=$((header_bytes + output_width * output_height * 3))
            fi
        fi
        if (( expected_output_bytes > 0 && output_bytes >= expected_output_bytes )); then
            status=0
            break
        fi
        if ! kill -0 "$player_pid" 2>/dev/null; then
            wait "$player_pid"
            status=$?
            break
        fi
        sleep 0.1
    done
    if kill -0 "$player_pid" 2>/dev/null; then
        kill "$player_pid" 2>/dev/null
        wait "$player_pid" 2>/dev/null
    fi
    set -e
    if (( status != 0 )); then
        printf 'Player failed for %s (status %s); see %s\n' "$path" "$status" "$log" >&2
        continue
    fi
    if [[ ! -s "$output_ppm" ]]; then
        printf 'No captured output for %s; see %s\n' "$path" "$log" >&2
        continue
    fi

    magick "$output_ppm" "$output_png"
    actual_dimensions="$(identify -format '%w %h' "$output_png")"
    if [[ "$actual_dimensions" != "$output_width $output_height" ]]; then
        printf 'Captured output dimensions changed for %s: expected %sx%s, got %s\n' \
            "$path" "$output_width" "$output_height" "$actual_dimensions" >&2
        continue
    fi
    reference_png="$frames/${clip_id}_reference_${output_width}x${output_height}_f${frame_index}.png"
    if [[ ! -s "$reference_png" ]]; then
        ffmpeg -hide_banner -loglevel error -i "$reference" \
            -vf "select=eq(n\\,${frame_index}),scale=${output_width}:${output_height}:flags=lanczos" \
            -frames:v 1 "$reference_png"
    fi
    ffmpeg -hide_banner -loglevel error -i "$path" \
        -vf "select=eq(n\\,${frame_index}),scale=${output_width}:${output_height}:flags=lanczos" \
        -frames:v 1 "$lanczos_png"
    magick "$output_png" "$reference_png" -compose difference -composite \
        -auto-level "$difference_png"

    metric_log="$logs/${stem}${tag_suffix}_metrics.log"
    ffmpeg -hide_banner -i "$output_png" -i "$reference_png" \
        -lavfi '[0:v][1:v]psnr' -f null - > /dev/null 2> "$metric_log"
    ffmpeg -hide_banner -i "$output_png" -i "$reference_png" \
        -lavfi '[0:v][1:v]ssim' -f null - > /dev/null 2>> "$metric_log"
    edge_log="$logs/${stem}${tag_suffix}_edge_metrics.log"
    ffmpeg -hide_banner -i "$output_png" -i "$reference_png" \
        -filter_complex \
        '[0:v]format=gray,edgedetect=low=0.05:high=0.15[dist];[1:v]format=gray,edgedetect=low=0.05:high=0.15[ref];[dist][ref]ssim' \
        -f null - > /dev/null 2> "$edge_log"

    lanczos_log="$logs/${stem}${tag_suffix}_lanczos_metrics.log"
    ffmpeg -hide_banner -i "$lanczos_png" -i "$reference_png" \
        -lavfi '[0:v][1:v]psnr' -f null - > /dev/null 2> "$lanczos_log"
    ffmpeg -hide_banner -i "$lanczos_png" -i "$reference_png" \
        -lavfi '[0:v][1:v]ssim' -f null - > /dev/null 2>> "$lanczos_log"
    lanczos_edge_log="$logs/${stem}${tag_suffix}_lanczos_edge_metrics.log"
    ffmpeg -hide_banner -i "$lanczos_png" -i "$reference_png" \
        -filter_complex \
        '[0:v]format=gray,edgedetect=low=0.05:high=0.15[dist];[1:v]format=gray,edgedetect=low=0.05:high=0.15[ref];[dist][ref]ssim' \
        -f null - > /dev/null 2> "$lanczos_edge_log"

    psnr="$(metric_value 'average' "$metric_log")"
    ssim="$(metric_value 'All' "$metric_log")"
    edge_ssim="$(metric_value 'All' "$edge_log")"
    lanczos_psnr="$(metric_value 'average' "$lanczos_log")"
    lanczos_ssim="$(metric_value 'All' "$lanczos_log")"
    lanczos_edge_ssim="$(metric_value 'All' "$lanczos_edge_log")"
    ssim_delta="$(awk -v fsr="$ssim" -v base="$lanczos_ssim" 'BEGIN { printf "%.6f", fsr - base }')"
    edge_ssim_delta="$(awk -v fsr="$edge_ssim" -v base="$lanczos_edge_ssim" 'BEGIN { printf "%.6f", fsr - base }')"
    scale="$(awk -v source="$width" -v output="$output_width" \
        'BEGIN { printf "%.3f", output / source }')"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$clip_id" "$width" "$height" "$output_width" "$output_height" "$scale" \
        "$quality" "$crf" "$frame_index" \
        "$psnr" "$ssim" "$edge_ssim" \
        "$lanczos_psnr" "$lanczos_ssim" "$lanczos_edge_ssim" \
        "$ssim_delta" "$edge_ssim_delta" "$output_png" "$difference_png" \
        >> "$results"
done
exec 3<&-

printf 'Quality results: %s\n' "$results"
