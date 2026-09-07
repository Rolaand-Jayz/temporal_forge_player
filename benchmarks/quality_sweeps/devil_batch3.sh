#!/usr/bin/env bash
# devil_batch3.sh — resolution/scene coverage + single-history isolation.
# All runs on the native INT8 path. Scenes/tiers per Luna's campaign matrix.
set -uo pipefail

QS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$QS_DIR/../.." && pwd)"
LUNA=/mnt/workdrive/ZCodeProject/temporal_forge_player
CLIPS="$LUNA/benchmarks/video_corpus/clips"
REFS="$LUNA/benchmarks/video_corpus/references"
OUTROOT="$QS_DIR/.devil_batch3"
CAP="$QS_DIR/devil_capture.sh"

export TFORGE_TEMPORAL_START_FRAME=0
export TFORGE_TEMPORAL_ANALYSIS_FRAME_INDICES=0,1,2,3,4,5,6,7

# name TIERDIR INPUT REF ; extra env via EXT_ENV
run() {
  local name="$1" inp="$2" ref="$3"; shift 3
  local status=ok
  env "$@" TFORGE_EXPERIMENT_ID="devil_b3_$name" \
    "$CAP" "$inp" "$ref" "$OUTROOT/$name" 8 8 \
    >"$OUTROOT/${name}.stdout" 2>&1 || status="exit:$?"
  echo "[$name] $status"
}

mkdir -p "$OUTROOT"

# ---- sintel_cave 720p (her one hidden reversal cell) ----
CAVE_IN="$CLIPS/sintel_cave_1280x720_high_crf12.mp4"
CAVE_REF="$CLIPS/sintel_cave_1920x1080_high_crf12.mp4"   # documented fallback ref
run cave_zero    "$CAVE_IN" "$CAVE_REF" TFORGE_FSR4_MOTION_ABLATION=zero
run cave_offline "$CAVE_IN" "$CAVE_REF" TFORGE_FSR4_MOTION_ABLATION=zero
run cave_t000    "$CAVE_IN" "$CAVE_REF" TFORGE_FSR4_MOTION_ABLATION=codec \
                 TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.0001 \
                 TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.0001
run cave_spatial "$CAVE_IN" "$CAVE_REF" TFORGE_FSR4_MOTION_ABLATION=zero \
                 TFORGE_FSR4_DISABLE_BEST_FINDINGS=1

# ---- rooftop 240p ----
P240_IN="$CLIPS/sintel_rooftop_426x240_high_crf12.mp4"
P240_REF="$REFS/sintel_rooftop_2160p_lossless.mkv"
run p240_zero    "$P240_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=zero
run p240_codec   "$P240_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=codec
run p240_t000    "$P240_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=codec \
                 TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.0001 \
                 TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.0001
run p240_t099    "$P240_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=codec \
                 TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.99 \
                 TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.99
run p240_offline "$P240_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=zero
run p240_spatial "$P240_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=zero \
                 TFORGE_FSR4_DISABLE_BEST_FINDINGS=1

# ---- rooftop 1080p (her C1 fixed tier) ----
P1080_IN="$CLIPS/sintel_rooftop_1920x1080_high_crf12.mp4"
run p1080_zero   "$P1080_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=zero
run p1080_codec  "$P1080_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=codec
run p1080_t000   "$P1080_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=codec \
                 TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.0001 \
                 TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.0001
run p1080_t099   "$P1080_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=codec \
                 TFORGE_FSR4_HISTORY_CONFIDENCE_THRESHOLD=0.99 \
                 TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE=0.99
run p1080_offline "$P1080_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=zero
run p1080_spatial "$P1080_IN" "$P240_REF" TFORGE_FSR4_MOTION_ABLATION=zero \
                  TFORGE_FSR4_DISABLE_BEST_FINDINGS=1

# ---- single-history isolation at 720p ----
run singlehist   "$CLIPS/sintel_rooftop_1280x720_high_crf12.mp4" "$P240_REF" \
                 TFORGE_FSR4_MOTION_ABLATION=zero \
                 TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND=1

# ---- offline sidecars for the new scene/tier combos ----
sidecar() {
  local dir="$1" inp="$2"
  local d="$OUTROOT/$dir"
  if [[ -f "$d/offline_dense_replay.json" ]]; then return 0; fi
  mkdir -p "$d"
  (cd "$REPO_ROOT" && python3 tools/validate_dense_motion.py \
    --input "$inp" --output "$d/offline_dense_report.json" \
    --flow-output "$d/offline_dense_flow.npz" \
    --replay-output "$d/offline_dense_replay.json" \
    --method farneback --frames 8) >/dev/null 2>&1 \
    && echo "[sidecar:$dir] built" || echo "[sidecar:$dir] FAILED"
}
sidecar cave_offline "$CAVE_IN"
sidecar p240_offline "$P240_IN"
sidecar p1080_offline "$P1080_IN"

DENSE_ENV() { printf 'TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION=%s' "$1"; }

# re-run the three offline arms now that sidecars exist
for combo in "cave_offline:$CAVE_IN:$CAVE_REF" "p240_offline:$P240_IN:$P240_REF" "p1080_offline:$P1080_IN:$P240_REF"; do
  IFS=: read -r name inp ref <<<"$combo"
  sc="$OUTROOT/$name/offline_dense_replay.json"
  if [[ -f "$sc" ]]; then
    run "$name" "$inp" "$ref" TFORGE_FSR4_MOTION_ABLATION=zero \
        "TFORGE_FSR4_EXPERIMENTAL_DENSE_MOTION=$sc"
  else
    echo "[$name] skipped (no sidecar)"
  fi
done

echo "batch3 done"
