#!/usr/bin/env bash
set -euo pipefail

# Bounded P0 diagnostic: two controls plus one source→model resize ablation.
# Never reuses a run directory; this deliberately does not invoke the campaign.
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
run_root="${1:-$repo/benchmarks/quality_sweeps/lattice_corruption_diagnostic/runs/$(date -u +%Y%m%dT%H%M%SZ)}"
[[ ! -e "$run_root" ]] || { echo "refusing to overwrite $run_root" >&2; exit 2; }
mkdir -p "$run_root"

common=(TFORGE_QUALITY_MANIFEST="$repo/benchmarks/video_corpus/manifest.csv" TFORGE_QUALITY_CLIP='^sintel_cave$' TFORGE_QUALITY_QUALITY=high TFORGE_QUALITY_FRAME=48 TFORGE_QUALITY_CAPTURE_ATTEMPTS=180 TFORGE_FSR4_FORCE_SCALE=2.00 TFORGE_FSR4_CAS_STRENGTH=0.00 TFORGE_FSR4_DISABLE_CAS=1 TFORGE_QUALITY_PROFILE=AMD_SEMANTIC_BASELINE TFORGE_FSR4_INTEGRATED_BEST_FINDINGS=1 TFORGE_FSR4_ENABLE_COLOR_HISTORY=1 TFORGE_FSR4_ENABLE_RECURRENT=1 TFORGE_FSR4_EXPERIMENTAL_SOURCE_TAP_MULAW=1 TFORGE_FSR4_DUMP_MODEL_INPUT=1 TFORGE_FSR4_DUMP_MODEL_INPUT_FRAME=48 TFORGE_FSR4_DUMP_PREFINAL=1 TFORGE_FSR4_DUMP_PREPASS_INPUT=1 TFORGE_FSR4_DUMP_DECODER=1 TFORGE_FSR4_DUMP_DECODER_FRAME=48)
run_case() {
  local name="$1" selector="$2" viewport="$3" reference="${4:-0}" root="$run_root/$1"
  mkdir -p "$root/stages"
  local -a extra=()
  (( reference )) && extra+=(TFORGE_FSR4_REFERENCE_RESIZE=1)
  env "${common[@]}" "${extra[@]}" TFORGE_QUALITY_ARTIFACT_ROOT="$root" TFORGE_QUALITY_FRAMES_DIR="$root/frames" TFORGE_QUALITY_LOGS_DIR="$root/logs" TFORGE_FSR4_DUMP_STAGE_DIR="$root/stages" TFORGE_FSR4_FORCE_VIEWPORT="$viewport" TFORGE_QUALITY_TAG="$name" bash "$repo/benchmarks/video_corpus/run_quality.sh" "$repo/build-fast/temporal_forge_player" "$selector" "$root/results.csv"
}
run_case bad_gpu 1280x720 1920x1080
run_case healthy_gpu 640x360 1280x720
run_case bad_reference 1280x720 1920x1080 1
printf '%s\n' "$run_root" > "$repo/benchmarks/quality_sweeps/lattice_corruption_diagnostic/LATEST_RUN"
echo "diagnostic artifacts: $run_root"
