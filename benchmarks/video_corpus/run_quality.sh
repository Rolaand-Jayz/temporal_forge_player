#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
binary="${1:-$repo/build-fast/temporal_forge_player}"
selector="${2:-1280x720}"
results="${3:-$root/results/quality.csv}"
asset_manifest="${TFORGE_QUALITY_ASSET_MANIFEST:-${results%.csv}.assets.csv}"
frame_index="${TFORGE_QUALITY_FRAME:-48}"
capture_attempts="${TFORGE_QUALITY_CAPTURE_ATTEMPTS:-120}"
tag="${TFORGE_QUALITY_TAG:-}"
preset="${TFORGE_BENCHMARK_PRESET:-saved}"
clip_filter="${TFORGE_QUALITY_CLIP:-}"
quality_filter="${TFORGE_QUALITY_QUALITY:-}"
corpus_manifest="${TFORGE_QUALITY_MANIFEST:-$root/manifest.csv}"
review_pipeline_config="${TFORGE_REVIEW_PIPELINE_CONFIG:-$root/review_pipeline.env}"
spatial_input="${TFORGE_QUALITY_SPATIAL_INPUT:-}"
# Preserve caller-owned experiment controls across the optional review config.
# That config contains defaults for interactive review, but must never replace
# an explicit arm value (the previous behavior changed requested CAS .20 to
# the config's .04 after the runner had recorded .20).
caller_review_fsr_cas="${TFORGE_REVIEW_FSR_CAS:-}"
caller_fsr4_cas_strength="${TFORGE_FSR4_CAS_STRENGTH:-}"
caller_git_head="${TFORGE_GIT_HEAD:-}"
caller_git_dirty="${TFORGE_GIT_DIRTY:-}"
if [[ -f "$review_pipeline_config" ]]; then
    # shellcheck disable=SC1090
    source "$review_pipeline_config"
fi
# Capture this opt-out before clearing the inherited variable. The single
# frame runner uses the same player switch, and each capture must record an
# explicit value rather than silently inheriting a previous campaign arm.
disable_best_findings="${TFORGE_FSR4_DISABLE_BEST_FINDINGS:-}"
unset TFORGE_FSR4_DISABLE_BEST_FINDINGS

# Quality captures must not inherit the interactive user's persisted settings.
# Upstream: the checked-in neutral benchmark settings below. Downstream: every
# player process in this run reads the temporary XDG config tree instead of
# ~/.config/temporal-forge-player/settings.json. This is capture isolation only;
# it does not change the player's normal interactive defaults.
benchmark_config_home="$(mktemp -d "${TMPDIR:-/tmp}/tforge-quality-config.XXXXXX")"
spatial_map=""
cleanup() {
    rm -rf "$benchmark_config_home"
    if [[ -n "$spatial_map" ]]; then
        rm -f "$spatial_map"
    fi
}
trap cleanup EXIT
mkdir -p "$benchmark_config_home/temporal-forge-player"
cp "$root/benchmark_settings.json" \
    "$benchmark_config_home/temporal-forge-player/settings.json"
export XDG_CONFIG_HOME="$benchmark_config_home"

if [[ -n "$tag" && ! "$tag" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    printf 'TFORGE_QUALITY_TAG contains unsupported filename characters: %s\n' "$tag" >&2
    exit 1
fi
tag_suffix="${tag:+_$tag}"
# Captures normally stay beside the corpus, but a review run can point both
# screenshots and logs at a durable external volume. Upstream: the caller's
# artifact-root choice. Downstream: every frame/log path in this run. This is
# storage routing only; it does not alter the player or image-processing path.
artifact_root="${TFORGE_QUALITY_ARTIFACT_ROOT:-$root/results}"
frames="${TFORGE_QUALITY_FRAMES_DIR:-$artifact_root/quality_frames}"
logs="${TFORGE_QUALITY_LOGS_DIR:-$artifact_root/quality_logs}"

if [[ ! -x "$binary" ]]; then
    printf 'Player binary is not executable: %s\n' "$binary" >&2
    exit 1
fi
[[ "$capture_attempts" =~ ^[1-9][0-9]*$ ]] || {
    printf 'TFORGE_QUALITY_CAPTURE_ATTEMPTS must be a positive integer: %s\n' "$capture_attempts" >&2
    exit 1
}
# The player is launched from a background environment command below. Make
# relative paths absolute so the launch does not depend on that child shell's
# working directory.
binary="$(realpath "$binary")"

mkdir -p "$frames" "$logs" "$(dirname "$results")" "$(dirname "$asset_manifest")"
if [[ -n "$spatial_input" ]]; then
    if [[ ! -s "$spatial_input" ]]; then
        printf 'Spatial capture input does not exist or is empty: %s\n' "$spatial_input" >&2
        exit 1
    fi
    spatial_map="$(mktemp "${TMPDIR:-/tmp}/tforge-spatial-map.XXXXXX.tsv")"
    if ! python3 "$repo/benchmarks/quality_sweeps/spatial_capture.py" \
        --input "$spatial_input" --output "$spatial_map"; then
        printf 'Spatial capture input validation failed: %s\n' "$spatial_input" >&2
        exit 1
    fi
fi
printf '%s\n' \
    'clip_id,preset,width,height,output_width,output_height,scale,quality,crf,frame,fsr_psnr_db,fsr_ssim,fsr_edge_ssim,lanczos_psnr_db,lanczos_ssim,lanczos_edge_ssim,bicubic_psnr_db,bicubic_ssim,bicubic_edge_ssim,fsr_vs_lanczos_ssim_delta,fsr_vs_lanczos_edge_ssim_delta,fsr_vs_bicubic_ssim_delta,fsr_vs_bicubic_edge_ssim_delta,fsr_lowfreq_luma_mae,fsr_lowfreq_luma_bias,class,output_path,full_output_path,difference_path,control_source_path,control_source_sha256' \
    > "$results"
printf '%s\n' 'scene,frame,path,width,height' > "$asset_manifest"
selected_clips=0
captured_rows=0
failed_captures=0

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
    selected_clips=$((selected_clips + 1))

    stem="$(basename "${path%.*}")"
    output_ppm="$frames/${stem}_f${frame_index}${tag_suffix}.ppm"
    output_png="$frames/${stem}_f${frame_index}${tag_suffix}.png"
    source_raw_ppm="$frames/${stem}_f${frame_index}${tag_suffix}_gpu_raw.ppm"
    source_raw_png="$frames/${stem}_f${frame_index}${tag_suffix}_gpu_raw.png"
    lanczos_png="$frames/${stem}_f${frame_index}${tag_suffix}_lanczos.png"
    bicubic_png="$frames/${stem}_f${frame_index}${tag_suffix}_bicubic.png"
    difference_png="$frames/${stem}_f${frame_index}${tag_suffix}_difference.png"
    log="$logs/${stem}${tag_suffix}.log"

    printf 'Capturing %s %sx%s %s frame %s...\n' \
        "$clip_id" "$width" "$height" "$quality" "$frame_index"
    # Never destroy an existing capture before its provenance has been
    # validated. Campaign callers must use a fresh artifact namespace; a
    # deliberate overwrite requires removing that namespace after preserving
    # its data snapshot.
    for existing_path in "$output_ppm" "$output_png" "$source_raw_ppm" \
        "$source_raw_png" "$lanczos_png" "$bicubic_png" "$difference_png"; do
        if [[ -e "$existing_path" || -L "$existing_path" ]]; then
            printf 'refusing to overwrite existing capture artifact: %s\n' \
                "$existing_path" >&2
            exit 2
        fi
    done
    set +e
    benchmark_env=()
    if [[ "$preset" != "saved" ]]; then
        benchmark_env+=("TFORGE_BENCHMARK_PRESET=$preset")
    fi
    if [[ -n "$disable_best_findings" ]]; then
        benchmark_env+=("TFORGE_FSR4_DISABLE_BEST_FINDINGS=$disable_best_findings")
    fi
    # Keep benchmark runs reproducible while allowing controlled experiments
    # with runtime quality/performance switches.  Do not pass the caller's
    # entire environment through: unrelated desktop settings can change the
    # player and make corpus results incomparable.
    for name in \
        TFORGE_BENCHMARK_SHARPNESS \
        TFORGE_BENCHMARK_JITTER_STRENGTH \
        TFORGE_FSR4_RE_ROOT \
        TFORGE_FSR4_POSTPASS_TRACE \
        TFORGE_FSR4_LEARNED_KERNEL_RADIUS \
        TFORGE_FSR4_LEARNED_KERNEL_SIGMA \
        TFORGE_FSR4_LEARNED_KERNEL_EXPONENT \
        TFORGE_FSR4_LEARNED_KERNEL_NORMALIZATION \
        TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT \
        TFORGE_FSR4_POSTPASS_TAIL_MAPPING \
        TFORGE_FSR4_POSTPASS_REVERSE_TAIL_CHANNELS \
        TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_RADIUS \
        TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_SIGMA \
        TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_WIDE_EXPONENT \
        TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_LEGACY_NORMALIZATION \
        TFORGE_FSR4_EXPERIMENTAL_LEARNED_KERNEL_RAW_NORMALIZATION \
        TFORGE_FSR4_EXPERIMENTAL_POSTPASS_CURRENT_WEIGHT \
        TFORGE_FSR4_EXPERIMENTAL_POSTPASS_SWAP_TAIL_MAPPING \
        TFORGE_FSR4_EXPERIMENTAL_POSTPASS_REVERSE_TAIL_CHANNELS \
        TFORGE_FSR4_CHAIN_PASSES \
        TFORGE_FSR4_TRUE_FSR1_EASU \
        TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND \
        TFORGE_FSR4_EXPERIMENTAL_LEGACY_ROUND \
        TFORGE_FSR4_EXPERIMENTAL_LEGACY_RECURRENT_BIAS \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING \
        TFORGE_FSR4_MOTION_ESTIMATOR \
        TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION \
        TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_SIGN \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_SCALE \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_ROUNDING \
        TFORGE_FSR4_EXPERIMENTAL_HISTORY_INTERPOLATION \
        TFORGE_FSR4_EXPERIMENTAL_RECURRENT_RESET_ONLY \
        TFORGE_FSR4_EXPERIMENTAL_BASE_UNJITTERED \
        TFORGE_FSR4_EXPERIMENTAL_DISABLE_POSTPASS_TAIL \
        TFORGE_FSR4_EXPERIMENTAL_RECOVERED_LINEAR_OUTPUT \
        TFORGE_FSR4_EXPERIMENTAL_REC709_INPUT_EOTF \
        TFORGE_FSR4_INPUT_TRANSFER \
        TFORGE_FSR4_INPUT_SHARPEN_STRENGTH \
        TFORGE_FSR4_CHROMA_FILTER \
        TFORGE_FSR4_CHROMA_PHASE \
        TFORGE_FSR4_EXPERIMENTAL_UNKNOWN_MATRIX_BT709 \
        TFORGE_FSR4_LEARNED_STRENGTH \
        TFORGE_FSR4_DISABLE_LEARNED_CONFIDENCE_GATE \
        TFORGE_FSR4_LEARNED_CONFIDENCE_BLEND \
        TFORGE_FSR4_LEARNED_CONFIDENCE_FLOOR \
        TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD \
        TFORGE_FSR4_MOTION_CONFIDENCE_REACTIVE \
        TFORGE_FSR4_EXPERIMENTAL_PHOTOMETRIC_HISTORY_GATE \
        TFORGE_FSR4_EXPERIMENTAL_UNJITTERED_MOTION_SAMPLE \
        TFORGE_FSR4_EXPERIMENTAL_JITTERED_MOTION_SAMPLE \
        TFORGE_FSR4_EXPERIMENTAL_CURRENT_INVALID_HISTORY \
        TFORGE_FSR4_EXPERIMENTAL_MOTION_MAX_BLOCKS \
        TFORGE_FSR4_ENABLE_HW_ANALYSIS_LUMA \
        TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION \
        TFORGE_FSR4_MOTION_REFINE_RADIUS \
        TFORGE_FSR4_MOTION_MAX_CORRECTION \
        TFORGE_FSR4_MOTION_MIN_ERROR_IMPROVEMENT \
        TFORGE_FSR4_MOTION_MIN_ERROR_MARGIN \
        TFORGE_FSR4_DUMP_MOTION_SIDECAR \
        TFORGE_FSR4_DUMP_MOTION_TEXTURE \
        TFORGE_FSR4_DUMP_REPROJECTED_COLOR \
        TFORGE_FSR4_DUMP_MOTION_SEEDS \
        TFORGE_FSR4_DUMP_DECODER \
        TFORGE_FSR4_DUMP_DECODER_FRAME \
        TFORGE_FSR4_DUMP_MODEL_INPUT \
        TFORGE_FSR4_DUMP_MODEL_INPUT_FRAME \
        TFORGE_FSR4_DUMP_PREFINAL \
        TFORGE_FSR4_DUMP_PREPASS_INPUT \
        TFORGE_FSR4_DUMP_STAGE_DIR \
        TFORGE_FSR4_REFERENCE_RESIZE \
        TFORGE_FSR4_DISABLE_MOTION_VALIDITY \
        TFORGE_FSR4_DRS \
        TFORGE_FSR4_FORCE_VIEWPORT \
        TFORGE_FSR4_FORCE_SCALE \
        TFORGE_FSR4_PROFILE_TIMINGS \
        TFORGE_FSR4_DISABLE_FUSED_INT8 \
        TFORGE_FSR4_ENABLE_FUSED_INT8 \
        TFORGE_FSR4_ENABLE_COLOR_HISTORY \
        TFORGE_FSR4_DISABLE_COLOR_HISTORY \
        TFORGE_FSR4_ENABLE_RECURRENT \
        TFORGE_FSR4_DISABLE_PREPASS \
        TFORGE_FSR4_CAS_STRENGTH \
        TFORGE_FSR4_PRE_CAS \
        TFORGE_FSR4_DISABLE_CAS \
        TFORGE_FSR4_LEGACY_RCAS_STRENGTH \
        TFORGE_FSR4_USE_DISPLAY_BASE \
    TFORGE_FSR4_DISPLAY_BASE_STRENGTH \
    TFORGE_FSR4_MOTION_AWARE_DISPLAY_BASE \
    TFORGE_FSR4_MOTION_AWARE_RESIDUAL \
    TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED \
    TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED_STRENGTH \
    TFORGE_FSR4_CURRENT_BASE_FILTER \
        TFORGE_FSR4_QUALITY_LAB_DISPLAY_BASE \
        TFORGE_FSR4_CURRENT_BLEND_LINEAR \
        TFORGE_FSR4_CURRENT_BASE_JITTERED \
        TFORGE_FSR4_FORCE_RESET \
        TFORGE_FSR4_DISABLE_POSTPASS \
        TFORGE_FSR4_DISABLE_NATIVE_INT8 \
        TFORGE_FSR4_DISABLE_COOP \
        TFORGE_FSR4_DISABLE_FP16_COOP \
        TFORGE_FSR4_DISABLE_FP16_DIRECT \
        TFORGE_FSR4_DISABLE_FP16_UPSCALE \
        TFORGE_FSR4_DISABLE_FP16_DOWNSCALE \
        TFORGE_FSR4_FP16_FINAL_SCALAR \
        TFORGE_FSR4_FP8_SCALE \
        TFORGE_FSR4_FP8_ROUNDING \
        TFORGE_FSR4_FP16_FP8_BOUNDARY \
        TFORGE_FSR4_COOP_MAX_STEP \
        TFORGE_FSR4_MAX_STEPS \
        TFORGE_FSR4_HDR_OUTPUT \
        TFORGE_DISABLE_HW_DECODE \
        TFORGE_FSR4_JITTER_MODE \
        TFORGE_FSR4_MOTION_ESTIMATOR \
        TFORGE_FSR4_INTEGRATED_TEMPORAL \
        TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE \
        TFORGE_FSR4_INTEGRATED_BEST_FINDINGS \
        TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER \
        TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING \
        TFORGE_FSR4_MOTION_ALLOW_B_FRAMES \
        TFORGE_FSR4_CONTROLLED_JITTER \
        TFORGE_FSR4_JITTER_SEQUENCE \
        TFORGE_FSR4_JITTER_CADENCE \
        TFORGE_QUALITY_LAB_CONFIG \
        TFORGE_QUALITY_PROFILE \
        TFORGE_EXPERIMENT_ID \
        TFORGE_RUNTIME_TRACE_PATH \
        TFORGE_GIT_HEAD \
        TFORGE_GIT_DIRTY \
        TFORGE_CONFIG_SHA256; do
        if [[ -n "${!name:-}" ]]; then
            benchmark_env+=("$name=${!name}")
        fi
    done
    # 426x240 is 1.775:1, so fitting it into a literal 1280x720 viewport
    # rounds to 1278x720. Request a one-pixel-wider 16:9 envelope; the
    # existing even-dimension fit then resolves to the intended 1280x720
    # output without changing the normal player path.
    if [[ "$selector" == "1280x720" && -z "${TFORGE_FSR4_FORCE_VIEWPORT:-}" ]]; then
        benchmark_env+=("TFORGE_FSR4_FORCE_VIEWPORT=1281x720")
    fi
    if [[ -n "$caller_review_fsr_cas" ]]; then
        benchmark_env+=("TFORGE_FSR4_CAS_STRENGTH=$caller_review_fsr_cas")
    elif [[ -n "$caller_fsr4_cas_strength" ]]; then
        benchmark_env+=("TFORGE_FSR4_CAS_STRENGTH=$caller_fsr4_cas_strength")
    elif [[ -n "${TFORGE_REVIEW_FSR_CAS:-}" ]]; then
        benchmark_env+=("TFORGE_FSR4_CAS_STRENGTH=${TFORGE_REVIEW_FSR_CAS}")
    fi
    env \
        "${benchmark_env[@]}" \
        TFORGE_HEADLESS_BENCHMARK=1 \
        TFORGE_FSR4_DUMP_OUTPUT=1 \
        TFORGE_FSR4_DUMP_RAW=1 \
        TFORGE_FSR4_DUMP_RAW_PATH="$source_raw_ppm" \
        TFORGE_FSR4_DUMP_OUTPUT_FRAME="$frame_index" \
        TFORGE_FSR4_DUMP_OUTPUT_PATH="$output_ppm" \
        "$binary" "$path" < /dev/null > "$log" 2>&1 &
    player_pid=$!
    status=124
    output_width=0
    output_height=0
    expected_output_bytes=0
    source_width=0
    source_height=0
    expected_source_bytes=0
    for ((attempt = 0; attempt < capture_attempts; ++attempt)); do
        output_bytes=0
        source_bytes=0
        output_magic=""
        output_max_value=""
        source_magic=""
        source_max_value=""
        output_complete=0
        source_complete=0
        if [[ -f "$output_ppm" ]]; then
            output_bytes="$(stat -c %s "$output_ppm")"
            output_magic="$(sed -n '1p' "$output_ppm" 2>/dev/null || true)"
            dimensions="$(sed -n '2p' "$output_ppm" 2>/dev/null || true)"
            output_max_value="$(sed -n '3p' "$output_ppm" 2>/dev/null || true)"
            if [[ "$dimensions" =~ ^([0-9]+)[[:space:]]+([0-9]+)$ ]]; then
                output_width="${BASH_REMATCH[1]}"
                output_height="${BASH_REMATCH[2]}"
                header_bytes="$(printf 'P6\n%s\n255\n' "$dimensions" | wc -c)"
                expected_output_bytes=$((header_bytes + output_width * output_height * 3))
            fi
        fi
        if [[ "$output_magic" == "P6" && "$output_max_value" == "255" ]] &&
           (( expected_output_bytes > 0 && output_bytes == expected_output_bytes )); then
            output_complete=1
        fi
        if [[ -f "$source_raw_ppm" ]]; then
            source_bytes="$(stat -c %s "$source_raw_ppm")"
            source_magic="$(sed -n '1p' "$source_raw_ppm" 2>/dev/null || true)"
            source_dimensions="$(sed -n '2p' "$source_raw_ppm" 2>/dev/null || true)"
            source_max_value="$(sed -n '3p' "$source_raw_ppm" 2>/dev/null || true)"
            if [[ "$source_dimensions" =~ ^([0-9]+)[[:space:]]+([0-9]+)$ ]]; then
                source_width="${BASH_REMATCH[1]}"
                source_height="${BASH_REMATCH[2]}"
                source_header_bytes="$(printf 'P6\n%s\n255\n' "$source_dimensions" | wc -c)"
                expected_source_bytes=$((source_header_bytes + source_width * source_height * 3))
            fi
        fi
        if [[ "$source_magic" == "P6" && "$source_max_value" == "255" ]] &&
           (( expected_source_bytes > 0 && source_bytes == expected_source_bytes )); then
            source_complete=1
        fi
        if (( output_complete && source_complete )); then
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
        failed_captures=$((failed_captures + 1))
        continue
    fi
    if [[ ! -s "$output_ppm" || ! -s "$source_raw_ppm" ]]; then
        printf 'Missing captured output or matched GPU source for %s; see %s\n' "$path" "$log" >&2
        failed_captures=$((failed_captures + 1))
        continue
    fi

    magick "$output_ppm" "$output_png"
    magick "$source_raw_ppm" "$source_raw_png"
    actual_dimensions="$(identify -format '%w %h' "$output_png")"
    if [[ "$actual_dimensions" != "$output_width $output_height" ]]; then
        printf 'Captured output dimensions changed for %s: expected %sx%s, got %s\n' \
            "$path" "$output_width" "$output_height" "$actual_dimensions" >&2
        failed_captures=$((failed_captures + 1))
        continue
    fi
    source_actual_dimensions="$(identify -format '%w %h' "$source_raw_png")"
    if [[ "$source_actual_dimensions" != "$width $height" ]]; then
        printf 'GPU source dimensions do not match manifest for %s: expected %sx%s, got %s\n' \
            "$path" "$width" "$height" "$source_actual_dimensions" >&2
        failed_captures=$((failed_captures + 1))
        continue
    fi
    # PNG encoders may add run-specific metadata, so hashing the container
    # would falsely make identical source pixels look different across
    # candidates. Hash canonical 8-bit RGB bytes instead; the CSV path still
    # identifies the lossless image used by the controls.
    control_source_sha256="$(magick "$source_raw_png" -alpha off -depth 8 RGB:- | sha256sum | cut -d' ' -f1)"
    if [[ -n "${TFORGE_QUALITY_OUTPUT_DIMENSIONS:-}" ]]; then
        expected_spatial_dimensions="${TFORGE_QUALITY_OUTPUT_DIMENSIONS/x/ }"
        if [[ "$actual_dimensions" != "$expected_spatial_dimensions" ]]; then
            printf 'Spatial capture output dimensions do not match input contract for %s: expected %s, got %s\n' \
                "$path" "$expected_spatial_dimensions" "$actual_dimensions" >&2
            failed_captures=$((failed_captures + 1))
            continue
        fi
    fi
    # Record the complete display image independently of class crops. A scene
    # with no selected metric region is still a valid human-review asset.
    printf '%s,%s,%s,%s,%s\n' \
        "$clip_id" "$frame_index" "$output_png" "$output_width" "$output_height" \
        >> "$asset_manifest"
    # Parallel candidate captures must not share this generated reference
    # image. Upstream: the immutable corpus reference video. Downstream: all
    # spatial metrics and difference images for this candidate. The tag keeps
    # one worker from reading another worker's partially written PNG.
    reference_png="$frames/${clip_id}_reference_${output_width}x${output_height}_f${frame_index}${tag_suffix}.png"
    if [[ ! -s "$reference_png" ]]; then
        ffmpeg -hide_banner -loglevel error -i "$reference" \
            -vf "select=eq(n\\,${frame_index}),scale=${output_width}:${output_height}:flags=lanczos" \
            -frames:v 1 "$reference_png"
    fi
    # Both spatial controls begin with the exact display-RGB source pixels
    # captured from the player's GPU decode at the measured frame. Decoding
    # the compressed clip a second time through FFmpeg would compare scaler
    # quality and decoder/chroma differences at once.
    ffmpeg -hide_banner -loglevel error -i "$source_raw_ppm" \
        -vf "scale=${output_width}:${output_height}:flags=lanczos" \
        -frames:v 1 "$lanczos_png"
    ffmpeg -hide_banner -loglevel error -i "$source_raw_ppm" \
        -vf "scale=${output_width}:${output_height}:flags=bicubic" \
        -frames:v 1 "$bicubic_png"
    magick "$output_png" "$reference_png" -compose difference -composite \
        -auto-level "$difference_png"

    if [[ -n "$spatial_input" ]]; then
        mapfile -t class_specs < <(awk -F '\t' -v scene="$clip_id" '$1 == scene { print }' "$spatial_map")
        if (( ${#class_specs[@]} == 0 )); then
            # A selected real scene may intentionally have no selected class.
            # It contributes no spatial row and is never expanded to a whole-scene row.
            continue
        fi
    else
        class_specs=("$clip_id"$'\t'"__whole_scene__"$'\t0\t0\t'"$output_width"$'\t'"$output_height")
    fi

    for class_spec in "${class_specs[@]}"; do
        IFS=$'\t' read -r _mapped_scene quality_class region_x region_y region_width region_height <<< "$class_spec"
        if [[ -n "$spatial_input" ]]; then
            class_suffix="${quality_class//[^A-Za-z0-9_.-]/_}"
            metric_output_png="$frames/${stem}_f${frame_index}${tag_suffix}_${class_suffix}.png"
            metric_reference_png="$frames/${clip_id}_reference_${output_width}x${output_height}_f${frame_index}${tag_suffix}_${class_suffix}.png"
            metric_lanczos_png="$frames/${stem}_f${frame_index}${tag_suffix}_${class_suffix}_lanczos.png"
            metric_bicubic_png="$frames/${stem}_f${frame_index}${tag_suffix}_${class_suffix}_bicubic.png"
            metric_difference_png="$frames/${stem}_f${frame_index}${tag_suffix}_${class_suffix}_difference.png"
            magick "$output_png" -crop "${region_width}x${region_height}+${region_x}+${region_y}" +repage "$metric_output_png"
            magick "$reference_png" -crop "${region_width}x${region_height}+${region_x}+${region_y}" +repage "$metric_reference_png"
            magick "$lanczos_png" -crop "${region_width}x${region_height}+${region_x}+${region_y}" +repage "$metric_lanczos_png"
            magick "$bicubic_png" -crop "${region_width}x${region_height}+${region_x}+${region_y}" +repage "$metric_bicubic_png"
            magick "$metric_output_png" "$metric_reference_png" -compose difference -composite \
                -auto-level "$metric_difference_png"
        else
            quality_class="__whole_scene__"
            metric_output_png="$output_png"
            metric_reference_png="$reference_png"
            metric_lanczos_png="$lanczos_png"
            metric_bicubic_png="$bicubic_png"
            metric_difference_png="$difference_png"
        fi
        metric_log="$logs/${stem}${tag_suffix}_${quality_class}_metrics.log"
        ffmpeg -hide_banner -i "$metric_output_png" -i "$metric_reference_png" \
            -lavfi '[0:v][1:v]psnr' -f null - > /dev/null 2> "$metric_log"
        ffmpeg -hide_banner -i "$metric_output_png" -i "$metric_reference_png" \
            -lavfi '[0:v][1:v]ssim' -f null - > /dev/null 2>> "$metric_log"
        edge_log="$logs/${stem}${tag_suffix}_${quality_class}_edge_metrics.log"
        ffmpeg -hide_banner -i "$metric_output_png" -i "$metric_reference_png" \
            -filter_complex \
            '[0:v]format=gray,edgedetect=low=0.05:high=0.15[dist];[1:v]format=gray,edgedetect=low=0.05:high=0.15[ref];[dist][ref]ssim' \
            -f null - > /dev/null 2> "$edge_log"

        lanczos_log="$logs/${stem}${tag_suffix}_${quality_class}_lanczos_metrics.log"
        ffmpeg -hide_banner -i "$metric_lanczos_png" -i "$metric_reference_png" \
            -lavfi '[0:v][1:v]psnr' -f null - > /dev/null 2> "$lanczos_log"
        ffmpeg -hide_banner -i "$metric_lanczos_png" -i "$metric_reference_png" \
            -lavfi '[0:v][1:v]ssim' -f null - > /dev/null 2>> "$lanczos_log"
        lanczos_edge_log="$logs/${stem}${tag_suffix}_${quality_class}_lanczos_edge_metrics.log"
        ffmpeg -hide_banner -i "$metric_lanczos_png" -i "$metric_reference_png" \
            -filter_complex \
            '[0:v]format=gray,edgedetect=low=0.05:high=0.15[dist];[1:v]format=gray,edgedetect=low=0.05:high=0.15[ref];[dist][ref]ssim' \
            -f null - > /dev/null 2> "$lanczos_edge_log"

        bicubic_log="$logs/${stem}${tag_suffix}_${quality_class}_bicubic_metrics.log"
        ffmpeg -hide_banner -i "$metric_bicubic_png" -i "$metric_reference_png" \
            -lavfi '[0:v][1:v]psnr' -f null - > /dev/null 2> "$bicubic_log"
        ffmpeg -hide_banner -i "$metric_bicubic_png" -i "$metric_reference_png" \
            -lavfi '[0:v][1:v]ssim' -f null - > /dev/null 2>> "$bicubic_log"
        bicubic_edge_log="$logs/${stem}${tag_suffix}_${quality_class}_bicubic_edge_metrics.log"
        ffmpeg -hide_banner -i "$metric_bicubic_png" -i "$metric_reference_png" \
            -filter_complex \
            '[0:v]format=gray,edgedetect=low=0.05:high=0.15[dist];[1:v]format=gray,edgedetect=low=0.05:high=0.15[ref];[dist][ref]ssim' \
            -f null - > /dev/null 2> "$bicubic_edge_log"

        # Measure low-frequency luminance separately from structural similarity.
        luma_output_log="$logs/${stem}${tag_suffix}_${quality_class}_luma_output.log"
        luma_reference_log="$logs/${stem}${tag_suffix}_${quality_class}_luma_reference.log"
        luma_difference_log="$logs/${stem}${tag_suffix}_${quality_class}_luma_difference.log"
        ffmpeg -hide_banner -i "$metric_output_png" \
            -vf 'format=gray,boxblur=lr=4:lp=1,signalstats,metadata=print' \
            -frames:v 1 -f null - > /dev/null 2> "$luma_output_log"
        ffmpeg -hide_banner -i "$metric_reference_png" \
            -vf 'format=gray,boxblur=lr=4:lp=1,signalstats,metadata=print' \
            -frames:v 1 -f null - > /dev/null 2> "$luma_reference_log"
        ffmpeg -hide_banner -i "$metric_output_png" -i "$metric_reference_png" \
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
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$clip_id" "$preset" "$width" "$height" "$output_width" "$output_height" "$scale" \
            "$quality" "$crf" "$frame_index" \
            "$psnr" "$ssim" "$edge_ssim" \
            "$lanczos_psnr" "$lanczos_ssim" "$lanczos_edge_ssim" \
            "$bicubic_psnr" "$bicubic_ssim" "$bicubic_edge_ssim" \
            "$ssim_delta" "$edge_ssim_delta" "$bicubic_ssim_delta" "$bicubic_edge_ssim_delta" \
            "$luma_mae" "$luma_bias" "$quality_class" \
            "$metric_output_png" "$output_png" "$metric_difference_png" \
            "$source_raw_png" "$control_source_sha256" \
            >> "$results"
        captured_rows=$((captured_rows + 1))
    done
done
exec 3<&-

if (( selected_clips == 0 )); then
    printf 'No corpus clips matched selector=%s clip_filter=%s quality_filter=%s\n' \
        "$selector" "${clip_filter:-<none>}" "${quality_filter:-<none>}" >&2
    exit 1
fi
if (( failed_captures != 0 || captured_rows == 0 )); then
    printf 'Quality capture incomplete: selected=%s captured=%s failed=%s; see %s\n' \
        "$selected_clips" "$captured_rows" "$failed_captures" "$logs" >&2
    exit 2
fi

printf 'Quality results: %s\n' "$results"
