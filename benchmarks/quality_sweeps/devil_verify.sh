#!/usr/bin/env bash
# devil_verify.sh — verification pass of the decisive batch-2 runs.
set -uo pipefail
QS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LUNA=/mnt/workdrive/ZCodeProject/temporal_forge_player
IN="$LUNA/benchmarks/video_corpus/clips/sintel_rooftop_1280x720_high_crf12.mp4"
REF="$LUNA/benchmarks/video_corpus/references/sintel_rooftop_2160p_lossless.mkv"
OUTROOT="$QS_DIR/.devil_verify"
CAP="$QS_DIR/devil_capture.sh"
export TFORGE_TEMPORAL_START_FRAME=0
export TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES=0,1,2,3,4,5,6,7
run() {
  local name="$1"; shift
  env "$@" TFORGE_EXPERIMENT_ID="devil_verify_$name" \
    "$CAP" "$IN" "$REF" "$OUTROOT/$name" 8 8 >"$OUTROOT/${name}.stdout" 2>&1 \
    && echo "[$name] ok" || echo "[$name] exit:$?"
}
mkdir -p "$OUTROOT"
run zero_def    TFORGE_FSR4_MOTION_ABLATION=zero
run codec_def   TFORGE_FSR4_MOTION_ABLATION=codec
run codec_t000  TFORGE_FSR4_MOTION_ABLATION=codec \
                TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.0001 \
                TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.0001
run codec_t099  TFORGE_FSR4_MOTION_ABLATION=codec \
                TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.99 \
                TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.99
run singlehist  TFORGE_FSR4_MOTION_ABLATION=zero \
                TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND=1
run spatial     TFORGE_FSR4_MOTION_ABLATION=zero \
                TFORGE_FSR4_DISABLE_BEST_FINDINGS=1
echo "verify done"
