#!/usr/bin/env bash
set -euo pipefail

# run_temporal_quality.sh — capture a short sequence and compare temporal
# stability against reference, Lanczos, and bilinear controls.
#
# Upstream: an executable player plus matched input/reference clips. Downstream:
# CSV metrics and optional frame/log artifacts. The script exercises temporal
# history and motion behavior but never edits the player or benchmark sources.

# Capture a short real FSR output sequence and compare it against a trusted
# Lanczos sequence. This complements the single-frame runner by exercising
# temporal accumulation, motion rejection, and lookahead.
#
# Usage: run_temporal_quality.sh PLAYER INPUT REFERENCE OUTPUT_CSV [FRAMES]

if (( $# < 4 || $# > 5 )); then
    printf 'usage: %s PLAYER INPUT REFERENCE OUTPUT_CSV [FRAMES]\n' "$0" >&2
    exit 2
fi

player="$(realpath "$1")"
input="$(realpath "$2")"
reference="$(realpath "$3")"
output="$4"
frames="${5:-8}"
artifact_dir="${TFORGE_TEMPORAL_ARTIFACT_DIR:-}"
[[ -x "$player" && -s "$input" && -s "$reference" ]] || {
    printf 'player, input, and reference must exist\n' >&2
    exit 1
}
[[ "$frames" =~ ^[1-9][0-9]*$ ]] || {
    printf 'frames must be a positive integer\n' >&2
    exit 2
}

for tool in ffmpeg identify; do
    command -v "$tool" >/dev/null || {
        printf 'required tool not found: %s\n' "$tool" >&2
        exit 1
    }
done

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
sequence_dir="$tmpdir/fsr"
mkdir -p "$sequence_dir"

set +e
TFORGE_HEADLESS_BENCHMARK=1 \
TFORGE_FSR4_DUMP_SEQUENCE="$frames" \
TFORGE_FSR4_DUMP_SEQUENCE_DIR="$sequence_dir" \
TFORGE_QUALITY_LAB_CONFIG="${TFORGE_QUALITY_LAB_CONFIG:-}" \
timeout 30s "$player" "$input" >"$tmpdir/player.log" 2>&1 &
pid=$!
status=124

sequence_complete() {
    # sequence_complete: validate that every requested PPM exists and has the
    # same complete P6 payload. This prevents metrics from starting on a partial
    # sequence while the headless player is still producing frames.
    local first="$sequence_dir/temporal_forge_fsr4_0000.ppm"
    [[ -s "$first" ]] || return 1
    local dimensions header_bytes expected_bytes width height
    dimensions="$(sed -n '2p' "$first" 2>/dev/null || true)"
    [[ "$dimensions" =~ ^([0-9]+)[[:space:]]+([0-9]+)$ ]] || return 1
    read -r width height <<< "$dimensions"
    header_bytes="$(printf 'P6\n%s\n255\n' "$dimensions" | wc -c)"
    expected_bytes=$((header_bytes + width * height * 3))
    for ((index = 0; index < frames; ++index)); do
        local path="$sequence_dir/temporal_forge_fsr4_$(printf '%04d' "$index").ppm"
        [[ -f "$path" ]] || return 1
        [[ "$(stat -c %s "$path")" -eq "$expected_bytes" ]] || return 1
    done
    return 0
}

for ((attempt = 0; attempt < 300; ++attempt)); do
    if sequence_complete; then
        status=0
        break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        status=$?
        break
    fi
    sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
fi
set -e
if (( status != 0 )); then
    printf 'player did not produce %s frames; see %s\n' "$frames" "$tmpdir/player.log" >&2
    exit 1
fi

first="$sequence_dir/temporal_forge_fsr4_0000.ppm"
dimensions="$(sed -n '2p' "$first")"
[[ "$dimensions" =~ ^[0-9]+[[:space:]]+[0-9]+$ ]] || {
    printf 'invalid FSR sequence header: %s\n' "$first" >&2
    exit 1
}
read -r output_width output_height <<< "$dimensions"

ffmpeg -hide_banner -loglevel error -framerate 30 \
    -i "$sequence_dir/temporal_forge_fsr4_%04d.ppm" -frames:v "$frames" \
    -c:v ffv1 -level 3 -g 1 "$tmpdir/fsr.mkv"
ffmpeg -hide_banner -loglevel error -i "$reference" -frames:v "$frames" \
    -vf "scale=${output_width}:${output_height}:flags=lanczos,setsar=1" \
    -an -c:v ffv1 -level 3 -g 1 "$tmpdir/reference.mkv"
ffmpeg -hide_banner -loglevel error -i "$input" -frames:v "$frames" \
    -vf "scale=${output_width}:${output_height}:flags=lanczos,setsar=1" \
    -an -c:v ffv1 -level 3 -g 1 "$tmpdir/lanczos.mkv"
ffmpeg -hide_banner -loglevel error -i "$input" -frames:v "$frames" \
    -vf "scale=${output_width}:${output_height}:flags=bilinear,setsar=1" \
    -an -c:v ffv1 -level 3 -g 1 "$tmpdir/bilinear.mkv"

mkdir -p "$(dirname "$output")"
ssim_log="$tmpdir/ssim.log"
ffmpeg -hide_banner -i "$tmpdir/fsr.mkv" -i "$tmpdir/reference.mkv" \
    -lavfi '[0:v][1:v]ssim=stats_file=/dev/stderr' -f null - 2> "$ssim_log"
fsr_ssim="$(awk -F'All:' '/All:/ {sum += $2; n++} END {if (n) printf "%.6f", sum/n; else print "0"}' "$ssim_log")"
lanczos_log="$tmpdir/lanczos_ssim.log"
ffmpeg -hide_banner -i "$tmpdir/lanczos.mkv" -i "$tmpdir/reference.mkv" \
    -lavfi '[0:v][1:v]ssim=stats_file=/dev/stderr' -f null - 2> "$lanczos_log"
lanczos_ssim="$(awk -F'All:' '/All:/ {sum += $2; n++} END {if (n) printf "%.6f", sum/n; else print "0"}' "$lanczos_log")"

temporal_delta_mean() {
    # temporal_delta_mean: measure frame-to-frame luma change after encoding a
    # sequence. The result is a stability signal, not a substitute for visual
    # inspection or spatial SSIM.
    local video="$1"
    local log="$2"
    ffmpeg -hide_banner -loglevel error -i "$video" \
        -vf "tblend=all_mode=difference,signalstats,metadata=print:file=${log}" \
        -f null - >/dev/null
    awk -F= '/lavfi.signalstats.YAVG=/ {sum += $2; n++} END {if (n) printf "%.6f", sum / n; else print "0"}' "$log"
}

fsr_delta_log="$tmpdir/fsr_temporal_delta.log"
reference_delta_log="$tmpdir/reference_temporal_delta.log"
lanczos_delta_log="$tmpdir/lanczos_temporal_delta.log"
bilinear_delta_log="$tmpdir/bilinear_temporal_delta.log"
fsr_delta="$(temporal_delta_mean "$tmpdir/fsr.mkv" "$fsr_delta_log")"
reference_delta="$(temporal_delta_mean "$tmpdir/reference.mkv" "$reference_delta_log")"
lanczos_delta="$(temporal_delta_mean "$tmpdir/lanczos.mkv" "$lanczos_delta_log")"
bilinear_delta="$(temporal_delta_mean "$tmpdir/bilinear.mkv" "$bilinear_delta_log")"
fsr_delta_error="$(awk -v a="$fsr_delta" -v b="$reference_delta" 'BEGIN { d = a - b; if (d < 0) d = -d; printf "%.6f", d }')"
lanczos_delta_error="$(awk -v a="$lanczos_delta" -v b="$reference_delta" 'BEGIN { d = a - b; if (d < 0) d = -d; printf "%.6f", d }')"
fsr_bilinear_delta_error="$(awk -v a="$fsr_delta" -v b="$bilinear_delta" 'BEGIN { d = a - b; if (d < 0) d = -d; printf "%.6f", d }')"
fsr_ssim_min="$(awk -F'All:' '/All:/ {value=$2; gsub(/^[[:space:]]+/, "", value); split(value, parts, " "); if (parts[1] != "" && (n == 0 || parts[1] < min)) min = parts[1]; n++} END {if (n) printf "%.6f", min; else print "0"}' "$ssim_log")"
lanczos_ssim_min="$(awk -F'All:' '/All:/ {value=$2; gsub(/^[[:space:]]+/, "", value); split(value, parts, " "); if (parts[1] != "" && (n == 0 || parts[1] < min)) min = parts[1]; n++} END {if (n) printf "%.6f", min; else print "0"}' "$lanczos_log")"

header='label,frames,output_width,output_height,fsr_ssim_mean,lanczos_ssim_mean,fsr_ssim_min,lanczos_ssim_min,fsr_temporal_delta_mean,reference_temporal_delta_mean,lanczos_temporal_delta_mean,fsr_temporal_delta_abs_error,lanczos_temporal_delta_abs_error,bilinear_temporal_delta_mean,fsr_temporal_delta_vs_bilinear_abs_error'
if [[ ! -s "$output" ]]; then
    printf '%s\n' "$header" > "$output"
elif [[ "$(head -n 1 "$output")" != "$header" ]]; then
    printf 'existing temporal result has an incompatible header: %s\n' "$output" >&2
    exit 1
fi
printf 'fsr,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$frames" "$output_width" "$output_height" "$fsr_ssim" "$lanczos_ssim" \
    "$fsr_ssim_min" "$lanczos_ssim_min" "$fsr_delta" "$reference_delta" \
    "$lanczos_delta" "$fsr_delta_error" "$lanczos_delta_error" \
    "$bilinear_delta" "$fsr_bilinear_delta_error" >> "$output"

if [[ -n "$artifact_dir" ]]; then
    mkdir -p "$artifact_dir/fsr_frames"
    cp "$sequence_dir"/temporal_forge_fsr4_*.ppm "$artifact_dir/fsr_frames/"
    cp "$tmpdir/fsr.mkv" "$tmpdir/reference.mkv" "$tmpdir/lanczos.mkv" \
        "$tmpdir/bilinear.mkv" "$artifact_dir/"
    cp "$ssim_log" "$lanczos_log" "$fsr_delta_log" "$reference_delta_log" \
        "$lanczos_delta_log" "$bilinear_delta_log" "$artifact_dir/"
fi
printf 'temporal quality results: %s\n' "$output"
