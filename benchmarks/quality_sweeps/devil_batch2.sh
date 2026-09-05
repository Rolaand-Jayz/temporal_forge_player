#!/usr/bin/env bash
# devil_batch2.sh — re-run of batch 1 on the NATIVE INT8 path.
# The native_i8 SPIR-V assets are now vendored in this worktree, so the
# player should select the production native graph (verify via s0z bit 12
# / "native-int8 graph complete" trace lines).
set -uo pipefail

QS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$QS_DIR/../.." && pwd)"
LUNA=/home/rolaandjayz/ZCodeProject/temporal_forge_player
IN="$LUNA/benchmarks/video_corpus/clips/sintel_rooftop_1280x720_high_crf12.mp4"
REF="$LUNA/benchmarks/video_corpus/references/sintel_rooftop_2160p_lossless.mkv"
OUTROOT="$QS_DIR/.devil_batch2"
CAP="$QS_DIR/devil_capture.sh"

export TFORGE_TEMPORAL_START_FRAME=0
export TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES=0,1,2,3,4,5,6,7

run() {
  local name="$1"; shift
  local status=ok
  env "$@" TFORGE_EXPERIMENT_ID="devil_b2_$name" \
    "$CAP" "$IN" "$REF" "$OUTROOT/$name" 8 8 \
    >"$OUTROOT/${name}.stdout" 2>&1 || status="exit:$?"
  echo "[$name] $status"
}

mkdir -p "$OUTROOT"

run zero_def    TFORGE_FSR4_MOTION_ABLATION=zero
run codec_def   TFORGE_FSR4_MOTION_ABLATION=codec TFORGE_FSR4_DUMP_MOTION_SEEDS=1
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

# Offline dense arm: build the replay sidecar with the repo tool.
DENSE_DIR="$OUTROOT/offline_def"
if [[ ! -f "$DENSE_DIR/offline_dense_replay.json" ]]; then
  mkdir -p "$DENSE_DIR"
  (cd "$REPO_ROOT" && python3 tools/validate_dense_motion.py \
    --input "$IN" --output "$DENSE_DIR/offline_dense_report.json" \
    --flow-output "$DENSE_DIR/offline_dense_flow.npz" \
    --replay-output "$DENSE_DIR/offline_dense_replay.json" \
    --method farneback --frames 8) \
    && echo "[sidecar] built" || echo "[sidecar] FAILED"
fi
if [[ -f "$DENSE_DIR/offline_dense_replay.json" ]]; then
  run offline_def TFORGE_FSR4_MOTION_ABLATION=zero \
                  TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION="$DENSE_DIR/offline_dense_replay.json"
else
  echo "[offline_def] skipped (no sidecar)"
fi

echo "batch2 done"
