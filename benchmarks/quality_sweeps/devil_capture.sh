#!/usr/bin/env bash
# devil_capture.sh — Devil's Advocate experimental capture wrapper.
#
# Runs the Devil branch build of the player through the production temporal
# path via run_temporal_quality.sh. Extra TFORGE_* overrides pass through from
# the caller's environment; arm selection is therefore fully explicit per run.
# Media may live in another checkout and is referenced read-only.
#
# usage: devil_capture.sh INPUT REFERENCE OUT_DIR [FRAMES] [WARMUP]
set -euo pipefail

DEVIL_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
PLAYER="${PLAYER:-$DEVIL_ROOT/build-devil/temporal_forge_player}"
RUNNER="$DEVIL_ROOT/benchmarks/video_corpus/run_temporal_quality.sh"
CONFIG="${CONFIG:-$DEVIL_ROOT/benchmarks/quality_sweeps/swarm/agent_recheck_current/config.json}"

IN="$1"; REF="$2"; OUT="$3"; FRAMES="${4:-8}"; WARMUP="${5:-8}"

mkdir -p "$OUT/artifacts"

exec env \
  TFORGE_QUALITY_LAB_CONFIG="$CONFIG" \
  TFORGE_QUALITY_PROFILE=AMD_SEMANTIC_BASELINE \
  TFORGE_BENCHMARK_PRESET=Quality \
  TFORGE_TEMPORAL_WARMUP_FRAMES="$WARMUP" \
  TFORGE_DISABLE_HW_DECODE=1 \
  TFORGE_TEMPORAL_CAPTURE_TIMEOUT=600 \
  TFORGE_FSR4_FORCE_VIEWPORT=1920x1080 \
  TFORGE_FSR4_FENCE_TIMEOUT_MS=15000 \
  TFORGE_FSR4_ENABLE_COLOR_HISTORY=1 \
  TFORGE_FSR4_ENABLE_RECURRENT=1 \
  TFORGE_PRESERVE_IMAGE_ARTIFACTS=1 \
  TFORGE_TEMPORAL_ARTIFACT_DIR="$OUT/artifacts" \
  TFORGE_FSR4_DUMP_MOTION_TEXTURE=1 \
  TFORGE_FSR4_DUMP_MOTION_SIDECAR=1 \
  TFORGE_FSR4_DUMP_REPROJECTED_COLOR=1 \
  TFORGE_FSR4_DUMP_EVENT_TRACE=1 \
  TFORGE_FSR4_DISPATCH_TRACE=1 \
  "$RUNNER" "$PLAYER" "$IN" "$REF" "$OUT/quality.csv" "$FRAMES"
