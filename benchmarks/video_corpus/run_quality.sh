#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
binary="${1:-$repo/build/temporal_forge_player}"
selector="${2:-1280x720}"
results="${3:-$root/results/quality.csv}"
frame_index="${TFORGE_QUALITY_FRAME:-48}"
tag="${TFORGE_QUALITY_TAG:-}"
preset="${TFORGE_BENCHMARK_PRESET:-saved}"
clip_filter="${TFORGE_QUALITY_CLIP:-}"
quality_filter="${TFORGE_QUALITY_QUALITY:-}"
corpus_manifest="${TFORGE_QUALITY_MANIFEST:-$root/manifest.csv}"
review_pipeline_config="${TFORGE_REVIEW_PIPELINE_CONFIG:-$root/review_pipeline.env}"
if [[ -f "$review_pipeline_config" ]]; then
    # shellcheck disable=SC1090
    source "$review_pipeline_config"
fi
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
# The player is launched from a background environment command below. Make
# relative paths absolute so the launch does not depend on that child shell's
# working directory.
binary="$(realpath "$binary")"

mkdir -p "$frames" "$logs" "$(dirname "$results")"
printf '%s\n' \
    'clip_id,preset,width,height,output_width,output_height,scale,quality,crf,frame,fsr_psnr_db,fsr_ssim,fsr_edge_ssim,lanczos_psnr_db,lanczos_ssim,lanczos_edge_ssim,bicubic_psnr_db,bicubic_ssim,bicubic_edge_ssim,fsr_vs_lanczos_ssim_delta,fsr_vs_lanczos_edge_ssim_delta,fsr_vs_bicubic_ssim_delta,fsr_vs_bicubic_edge_ssim_delta,fsr_lowfreq_luma_mae,fsr_lowfreq_luma_bias,output_path,difference_path' \
    > "$results"

metric_value() {
    # metric_value: extract the last scalar emitted by a tagged ffmpeg/player
    # log. The CSV writer uses the final value because setup diagnostics may
    # appear before the measured frame.
    local label="$1"
    local file="$2"
    grep -o "${label}:[-0-9.]*" "$file" | tail -1 | cut -d: -f2
}

signalstats_value() {
    # signalstats_value: read one named luma statistic from a metadata log.
    # This keeps spatial error metrics separate from timing and temporal tests.
    local label="$1"
    local file="$2"
    grep -o "lavfi.signalstats.${label}=[-0-9.]*" "$file" | tail -1 | cut -d= -f2
}

if [[ ! -s "$corpus_manifest" ]]; then
    printf 'Quality corpus manifest does not exist: %s\n' "$corpus_manifest" >&2
    exit 1
fi
exec 3< "$corpus_manifest"
IFS= read -r _header <&3
while IFS=, read -r \
    clip_id title source_url license start duration width height quality crf path reference <&3; do
    if [[ -z "$reference" || ! "$path" =~ $selector ]]; then
        continue
    fi
    if [[ -n "$clip_filter" && ! "$clip_id" =~ $clip_filter ]]; then
        continue
    fi
    if [[ -n "$quality_filter" && "$quality" != "$quality_filter" ]]; then
        continue
    fi

    stem="$(basename "${path%.*}")"
    output_ppm="$frames/${stem}_f${frame_index}${tag_suffix}.ppm"
    output_png="$frames/${stem}_f${frame_index}${tag_suffix}.png"
    lanczos_png="$frames/${stem}_f${frame_index}${tag_suffix}_lanczos.png"
    bicubic_png="$frames/${stem}_f${frame_index}${tag_suffix}_bicubic.png"
    difference_png="$frames/${stem}_f${frame_index}${tag_suffix}_difference.png"
    log="$logs/${stem}${tag_suffix}.log"

    printf 'Capturing %s %sx%s %s frame %s...\n' \
        "$clip_id" "$width" "$height" "$quality" "$frame_index"
    rm -f "$output_ppm" "$output_png" "$lanczos_png" "$bicubic_png" "$difference_png"
    set +e
    benchmark_env=()
    if [[ "$preset" != "saved" ]]; then
        benchmark_env+=("TFORGE_BENCHMARK_PRESET=$preset")
    fi
    # Keep benchmark runs reproducible while allowing controlled experiments
    # with runtime quality/performance switches.  Do not pass the caller's
    # entire environment through: unrelated desktop settings can change the
    # player and make corpus results incomparable.
    for name in \
        TFORGE_FSR4_LEARNED_STRENGTH \
        TFORGE_FSR4_DRS \
        TFORGE_FSR4_FORCE_VIEWPORT \
        TFORGE_FSR4_FORCE_SCALE \
        TFORGE_FSR4_DISABLE_FUSED_INT8 \
        TFORGE_FSR4_ENABLE_FUSED_INT8 \
        TFORGE_QUALITY_LAB_CONFIG; do
        if [[ -n "${!name:-}" ]]; then
            benchmark_env+=("$name=${!name}")
        fi
    done
    # 426x240 is 1.775:1, so fitting it into a literal 1280x720 viewport
    # rounds to 1278x720. Request a one-pixel-wider 16:9 envelope; the
    # existing even-dimension fit then resolves to the intended 1280x720
    # output without changing the normal player path.
    if [[ "$selector" == "1280x720" ]]; then
        benchmark_env+=("TFORGE_FSR4_FORCE_VIEWPORT=1281x720")
    fi
    if [[ -n "${TFORGE_REVIEW_FSR_CAS:-}" ]]; then
        benchmark_env+=("TFORGE_FSR4_CAS_STRENGTH=${TFORGE_REVIEW_FSR_CAS}")
    fi
    env \
        "${benchmark_env[@]}" \
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
    ffmpeg -hide_banner -loglevel error -i "$path" \
        -vf "select=eq(n\\,${frame_index}),scale=${output_width}:${output_height}:flags=bicubic" \
        -frames:v 1 "$bicubic_png"
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

    bicubic_log="$logs/${stem}${tag_suffix}_bicubic_metrics.log"
    ffmpeg -hide_banner -i "$bicubic_png" -i "$reference_png" \
        -lavfi '[0:v][1:v]psnr' -f null - > /dev/null 2> "$bicubic_log"
    ffmpeg -hide_banner -i "$bicubic_png" -i "$reference_png" \
        -lavfi '[0:v][1:v]ssim' -f null - > /dev/null 2>> "$bicubic_log"
    bicubic_edge_log="$logs/${stem}${tag_suffix}_bicubic_edge_metrics.log"
    ffmpeg -hide_banner -i "$bicubic_png" -i "$reference_png" \
        -filter_complex \
        '[0:v]format=gray,edgedetect=low=0.05:high=0.15[dist];[1:v]format=gray,edgedetect=low=0.05:high=0.15[ref];[dist][ref]ssim' \
        -f null - > /dev/null 2> "$bicubic_edge_log"

    # Measure low-frequency luminance separately from structural similarity.
    # The blur suppresses texture so an exposure/transfer mismatch is not
    # mistaken for recovered detail. The bias is signed output-minus-reference
    # in normalized luma; the MAE is unsigned and normalized to 0..1.
    luma_output_log="$logs/${stem}${tag_suffix}_luma_output.log"
    luma_reference_log="$logs/${stem}${tag_suffix}_luma_reference.log"
    luma_difference_log="$logs/${stem}${tag_suffix}_luma_difference.log"
    ffmpeg -hide_banner -i "$output_png" \
        -vf 'format=gray,boxblur=lr=4:lp=1,signalstats,metadata=print' \
        -frames:v 1 -f null - > /dev/null 2> "$luma_output_log"
    ffmpeg -hide_banner -i "$reference_png" \
        -vf 'format=gray,boxblur=lr=4:lp=1,signalstats,metadata=print' \
        -frames:v 1 -f null - > /dev/null 2> "$luma_reference_log"
    ffmpeg -hide_banner -i "$output_png" -i "$reference_png" \
        -lavfi '[0:v]format=gray,boxblur=lr=4:lp=1[a];[1:v]format=gray,boxblur=lr=4:lp=1[b];[a][b]blend=all_mode=difference,signalstats,metadata=print' \
        -frames:v 1 -f null - > /dev/null 2> "$luma_difference_log"

    psnr="$(metric_value 'average' "$metric_log")"
    ssim="$(metric_value 'All' "$metric_log")"
    edge_ssim="$(metric_value 'All' "$edge_log")"
    lanczos_psnr="$(metric_value 'average' "$lanczos_log")"
    lanczos_ssim="$(metric_value 'All' "$lanczos_log")"
    lanczos_edge_ssim="$(metric_value 'All' "$lanczos_edge_log")"
    bicubic_psnr="$(metric_value 'average' "$bicubic_log")"
    bicubic_ssim="$(metric_value 'All' "$bicubic_log")"
    bicubic_edge_ssim="$(metric_value 'All' "$bicubic_edge_log")"
    luma_output_avg="$(signalstats_value 'YAVG' "$luma_output_log")"
    luma_reference_avg="$(signalstats_value 'YAVG' "$luma_reference_log")"
    luma_difference_avg="$(signalstats_value 'YAVG' "$luma_difference_log")"
    luma_mae="$(awk -v value="$luma_difference_avg" 'BEGIN { printf "%.6f", value / 255.0 }')"
    luma_bias="$(awk -v output="$luma_output_avg" -v reference="$luma_reference_avg" \
        'BEGIN { printf "%.6f", (output - reference) / 255.0 }')"
    ssim_delta="$(awk -v fsr="$ssim" -v base="$lanczos_ssim" 'BEGIN { printf "%.6f", fsr - base }')"
    edge_ssim_delta="$(awk -v fsr="$edge_ssim" -v base="$lanczos_edge_ssim" 'BEGIN { printf "%.6f", fsr - base }')"
    bicubic_ssim_delta="$(awk -v fsr="$ssim" -v base="$bicubic_ssim" 'BEGIN { printf "%.6f", fsr - base }')"
    bicubic_edge_ssim_delta="$(awk -v fsr="$edge_ssim" -v base="$bicubic_edge_ssim" 'BEGIN { printf "%.6f", fsr - base }')"
    scale="$(awk -v source="$width" -v output="$output_width" \
        'BEGIN { printf "%.3f", output / source }')"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$clip_id" "$preset" "$width" "$height" "$output_width" "$output_height" "$scale" \
        "$quality" "$crf" "$frame_index" \
        "$psnr" "$ssim" "$edge_ssim" \
        "$lanczos_psnr" "$lanczos_ssim" "$lanczos_edge_ssim" \
        "$bicubic_psnr" "$bicubic_ssim" "$bicubic_edge_ssim" \
        "$ssim_delta" "$edge_ssim_delta" "$bicubic_ssim_delta" "$bicubic_edge_ssim_delta" \
        "$luma_mae" "$luma_bias" \
        "$output_png" "$difference_png" \
        >> "$results"
done
exec 3<&-

printf 'Quality results: %s\n' "$results"
