#!/usr/bin/env bash
# devil_batch1.sh — Devil's Advocate batch 1: temporal-consumption falsification.
# Sequential captures on the Devil build through the production temporal path.
# Each run is isolated under .devil_batch1/<name>/ with its own artifacts.
set -uo pipefail

DEVIL_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LUNA=/home/rolaandjayz/ZCodeProject/temporal_forge_player
IN="$LUNA/benchmarks/video_corpus/clips/sintel_rooftop_1280x720_high_crf12.mp4"
REF="$LUNA/benchmarks/video_corpus/references/sintel_rooftop_2160p_lossless.mkv"
OUTROOT="$DEVIL_ROOT/.devil_batch1"
CAP="$DEVIL_ROOT/devil_capture.sh"

# Luna's multiframe protocol, for exact comparability with her matrix.
export TFORGE_TEMPORAL_START_FRAME=0
export TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES=0,1,2,3,4,5,6,7

run() {
  local name="$1"; shift
  local status=ok
  env "$@" TFORGE_EXPERIMENT_ID="devil_b1_$name" \
    "$CAP" "$IN" "$REF" "$OUTROOT/$name" 8 8 \
    >"$OUTROOT/${name}.stdout" 2>&1 || status="exit:$?"
  echo "[$name] $status"
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
run cw100       TFORGE_FSR4_MOTION_ABLATION=zero \
                TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=1.0
run cw000       TFORGE_FSR4_MOTION_ABLATION=zero \
                TFORGE_FSR4_POSTPASS_CURRENT_WEIGHT=0.0
run spatial     TFORGE_FSR4_MOTION_ABLATION=zero \
                TFORGE_FSR4_DISABLE_BEST_FINDINGS=1
run refined_def TFORGE_FSR4_MOTION_ABLATION=refined \
                TFORGE_FSR4_DUMP_MOTION_SEEDS=1

# Offline dense arm needs a replay sidecar; build it with the repo tool.
DENSE_DIR="$OUTROOT/offline_def"
if [[ ! -f "$DENSE_DIR/offline_dense_replay.json" ]]; then
  mkdir -p "$DENSE_DIR"
  if python3 -c 'import cv2' 2>/dev/null; then
    (cd "$DEVIL_ROOT" && python3 tools/validate_dense_motion.py \
      --input "$IN" --output "$DENSE_DIR/offline_dense_report.json" \
      --flow-output "$DENSE_DIR/offline_dense_flow.npz" \
      --replay-output "$DENSE_DIR/offline_dense_replay.json" \
      --method farneback --frames 8) \
      && echo "[sidecar] built" || echo "[sidecar] FAILED"
  else
    echo "[sidecar] cv2 unavailable"
  fi
fi
if [[ -f "$DENSE_DIR/offline_dense_replay.json" ]]; then
  run offline_def TFORGE_FSR4_MOTION_ABLATION=zero \
                  TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION="$DENSE_DIR/offline_dense_replay.json"
else
  echo "[offline_def] skipped (no sidecar)"
fi

echo "batch1 done"
