#!/usr/bin/env bash
# devil_batch4.sh — 360p tier completion (all four campaign tiers covered).
set -uo pipefail
QS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LUNA=/home/rolaandjayz/ZCodeProject/temporal_forge_player
IN="$LUNA/benchmarks/video_corpus/clips/sintel_rooftop_640x360_high_crf12.mp4"
REF="$LUNA/benchmarks/video_corpus/references/sintel_rooftop_2160p_lossless.mkv"
OUTROOT="$QS_DIR/.devil_batch4"
CAP="$QS_DIR/devil_capture.sh"
export TFORGE_TEMPORAL_START_FRAME=0
export TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES=0,1,2,3,4,5,6,7
run() {
  local name="$1"; shift
  env "$@" TFORGE_EXPERIMENT_ID="devil_b4_$name" \
    "$CAP" "$IN" "$REF" "$OUTROOT/$name" 8 8 >"$OUTROOT/${name}.stdout" 2>&1 \
    && echo "[$name] ok" || echo "[$name] exit:$?"
}
mkdir -p "$OUTROOT"
run p360_zero    TFORGE_FSR4_MOTION_ABLATION=zero
run p360_codec   TFORGE_FSR4_MOTION_ABLATION=codec
run p360_spatial TFORGE_FSR4_MOTION_ABLATION=zero TFORGE_FSR4_DISABLE_BEST_FINDINGS=1
echo "batch4 done"
