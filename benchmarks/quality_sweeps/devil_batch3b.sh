#!/usr/bin/env bash
# devil_batch3b.sh — bound the postpass historyBlend weight on the CODEC arm.
# triangle: codec_def (full temporal) vs codec_cw000 (currentWeight=0, pure
# temporalModelColor) vs codec_singlehist (bit1024 bypass: temporalModelColor
# = upscaledColor). If cw000 == singlehist, historyBlend ~ 0 even with real
# motion reaching the prepass.
set -uo pipefail

QS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LUNA=/mnt/workdrive/ZCodeProject/temporal_forge_player
IN="$LUNA/benchmarks/video_corpus/clips/sintel_rooftop_1280x720_high_crf12.mp4"
REF="$LUNA/benchmarks/video_corpus/references/sintel_rooftop_2160p_lossless.mkv"
OUTROOT="$QS_DIR/.devil_batch3b"
CAP="$QS_DIR/devil_capture.sh"

export TFORGE_TEMPORAL_START_FRAME=0
export TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES=0,1,2,3,4,5,6,7

run() {
  local name="$1"; shift
  local status=ok
  env "$@" TFORGE_EXPERIMENT_ID="devil_b3b_$name" \
    "$CAP" "$IN" "$REF" "$OUTROOT/$name" 8 8 \
    >"$OUTROOT/${name}.stdout" 2>&1 || status="exit:$?"
  echo "[$name] $status"
}

mkdir -p "$OUTROOT"
run codec_cw000      TFORGE_FSR4_MOTION_ABLATION=codec \
                     TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.0
run codec_singlehist TFORGE_FSR4_MOTION_ABLATION=codec \
                     TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND=1
run codec_cw100      TFORGE_FSR4_MOTION_ABLATION=codec \
                     TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=1.0
echo "batch3b done"
