#!/usr/bin/env bash
set -euo pipefail

# Capture the review-only matrix described by review_best_finds.json.  The
# script runs the existing single-frame quality harness with explicit env
# settings, so it does not alter reconstruction defaults or benchmark files.
# Upstream: manifest clips and five measured arm definitions. Downstream:
# lossless PNGs copied into the human-facing review result pool.

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$root/../.." && pwd)"
player="${1:-$repo/build-fast/temporal_forge_player}"
campaign_root="${TFORGE_REVIEW_CAMPAIGN_ROOT:-/mnt/external/Temporal Forge/quality-campaign/review-best-finds-20260827}"
review_root="${TFORGE_REVIEW_OUTPUT_ROOT:-$repo/benchmarks/video_corpus/results/review_reconciled_540}"
workers="${TFORGE_REVIEW_WORKERS:-2}"
frame="${TFORGE_QUALITY_FRAME:-48}"
arm_filter="${TFORGE_REVIEW_ARM:-}"
force_recapture="${TFORGE_REVIEW_FORCE_RECAPTURE:-0}"
manifest="$root/review_best_finds.json"
review_config="$root/review_current_path.json"
combined_review_config="$root/review_best_findings_path.json"

[[ -x "$player" ]] || { printf 'player is not executable: %s\n' "$player" >&2; exit 1; }
command -v jq >/dev/null || { printf 'jq is required\n' >&2; exit 1; }
command -v magick >/dev/null || { printf 'ImageMagick is required\n' >&2; exit 1; }
[[ "$workers" =~ ^[1-9][0-9]*$ ]] || { printf 'TFORGE_REVIEW_WORKERS must be positive\n' >&2; exit 2; }
[[ -f "$review_config" ]] || { printf 'missing review capture config: %s\n' "$review_config" >&2; exit 1; }
[[ -f "$combined_review_config" ]] || { printf 'missing combined review capture config: %s\n' "$combined_review_config" >&2; exit 1; }
mkdir -p "$campaign_root" "$review_root"

# Keep the capture matrix driven by the same manifest that drives the UI. This
# is especially important for native-input and native-reference controls:
# every declared input/output pair must get a real file, or the reviewer will
# show a disabled button instead of silently inventing a filename.
mapfile -t inputs < <(jq -r '.inputs[]' "$manifest")
mapfile -t outputs < <(jq -r '.outputs[]' "$manifest")
mapfile -t scenes < <(jq -r '.scenes[]' "$manifest")
run_task() {
  local arm_id="$1" scene="$2" input_res="$3" output_res="$4"
  local clip="$root/clips/${scene}_${input_res}_high_crf12.mp4"
  local reference="$root/references/${scene}_2160p_lossless.mkv"
  local safe="${arm_id}_${scene}_${input_res:-none}_to_${output_res:-none}"
  # Never reuse a failed task directory. The underlying quality runner writes
  # several sidecars and may leave one behind when an auxiliary conversion
  # fails; a unique attempt directory prevents stale pixels or logs from
  # being mistaken for this run's output.
  local run_dir="$campaign_root/$safe/attempt-$(date +%s%N)"
  local csv="$run_dir/quality.csv"
  if [[ "$arm_id" == "native-output" ]]; then
    [[ -s "$reference" ]] || { printf 'missing reference for %s\n' "$safe" >&2; return 1; }
  else
    [[ -s "$clip" && -s "$reference" ]] || { printf 'missing input/reference for %s\n' "$safe" >&2; return 1; }
  fi
  local final_png
  if [[ "$arm_id" == "native-input" ]]; then
    final_png="$review_root/${scene}_input${input_res}_${arm_id}.png"
  elif [[ "$arm_id" == "native-output" ]]; then
    final_png="$review_root/${scene}_output${output_res}_${arm_id}.png"
  else
    final_png="$review_root/${scene}_input${input_res}_to${output_res}_${arm_id}.png"
  fi
  # Completed PNGs are the resume checkpoint. This matters after a worker
  # interruption: rerunning the matrix must not recapture or overwrite a
  # verified image with a different frame or runtime state.
  if [[ "$force_recapture" != "1" && -s "$final_png" ]]; then
    return 0
  fi

  # Native controls do not need to launch the player. The input control is a
  # decoded frame from the selected clip, and the reference control is the
  # lossless corpus frame rendered at the selected output size. Keeping these
  # controls on the direct media path avoids coupling a truthful native view
  # to an unrelated GPU capture failure.
  if [[ "$arm_id" == "native-input" || "$arm_id" == "native-output" || "$arm_id" == "native-reference" ]]; then
    local native_work="$campaign_root/native-${scene}-${input_res}-${output_res}"
    mkdir -p "$native_work"
    if [[ "$arm_id" == "native-input" ]]; then
      ffmpeg -y -hide_banner -loglevel error -i "$clip" \
        -vf "select=eq(n\\,${frame})" -frames:v 1 "$final_png"
    else
      ffmpeg -y -hide_banner -loglevel error -i "$reference" \
        -vf "select=eq(n\\,${frame}),scale=${output_res/x/:}:flags=lanczos" \
        -frames:v 1 "$final_png"
    fi
    [[ -s "$final_png" ]] || { printf 'no native image for %s\\n' "$safe" >&2; return 1; }
    return 0
  fi
  mkdir -p "$run_dir"
  local -a env_args=(
    TFORGE_QUALITY_CLIP="$scene"
    TFORGE_QUALITY_QUALITY=high
    TFORGE_QUALITY_FRAME="$frame"
    TFORGE_QUALITY_TAG="$arm_id"
    TFORGE_QUALITY_CAPTURE_ATTEMPTS="${TFORGE_QUALITY_CAPTURE_ATTEMPTS:-600}"
    TFORGE_QUALITY_ARTIFACT_ROOT="$run_dir/artifacts"
    TFORGE_QUALITY_MANIFEST="$root/manifest.csv"
    TFORGE_FSR4_FORCE_VIEWPORT="$output_res"
    # Match the game's single post-reconstruction display CAS stage. Native
    # input/reference controls bypass the player and therefore remain raw.
    # This is intentionally fixed for a distributable review set: changing
    # the shell environment must not make two reviewers judge different
    # sharpening policies.
    TFORGE_FSR4_CAS_STRENGTH="0.20"
    # The checked-in quality-lab file is a severe-upscale base-only
    # experiment. Review captures use an isolated current-composition config
    # so method labels correspond to the path that actually rendered pixels.
    TFORGE_QUALITY_LAB_CONFIG="$review_config"
  )
  # The combined temporal arms use the typed motion profile so their
  # confidence-aware causal motion and conservative missing-reference
  # fallback are reproducible in the captured pixels; all other arms retain
  # the ordinary review config. Edge-aware expansion remains a separate probe.
  if [[ "$arm_id" =~ ^(best-findings-temporal|fsr1-best-findings-temporal|best-findings-temporal-synthetic-jitter)$ ]]; then
    env_args+=(TFORGE_QUALITY_LAB_CONFIG="$combined_review_config")
  fi
  while IFS=$'\t' read -r name value; do
    env_args+=("$name=$value")
  done < <(jq -r --arg id "$arm_id" '.arms[] | select(.id==$id) | .env | to_entries[] | [.key,.value] | @tsv' "$manifest")
  # The manifest carries review labels and the ordinary arm environment, but
  # its older motion entries must not override the combined profile above.
  # Reassert the measured retained handoff after metadata is loaded so the
  # pixels selected by the reviewer use causal codec seeds, validation,
  # confidence, and integrated history/color ordering. The combined path also
  # enables only the conservative missing-reference fallback; refined motion
  # and edge-aware expansion remain separate probes because they were not
  # proven to improve the matched corpus.
  if [[ "$arm_id" =~ ^(best-findings-temporal|fsr1-best-findings-temporal|best-findings-temporal-synthetic-jitter)$ ]]; then
    env_args+=(
      TFORGE_FSR4_MOTION_ESTIMATOR="codec"
      TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING="1"
      TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE="0.0"
      TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE="1"
    )
  fi
  # Reassert the display contract after arm metadata has been loaded. This
  # prevents a future experiment entry from accidentally overriding the
  # reviewer-facing CAS policy or enabling a second chained CAS pass.
  env_args+=(
    TFORGE_FSR4_CAS_STRENGTH="0.20"
    TFORGE_FSR4_LEGACY_RCAS_STRENGTH="0"
    TFORGE_FSR4_CHAIN_PASSES="0"
  )
  env -u TFORGE_FSR4_DISABLE_CAS "${env_args[@]}" "$root/run_quality.sh" "$player" "$input_res" "$csv"
  local source_png
  # Temporal Forge is the player's captured output. The
  # fsr1 arm uses only the maintained FSR1/EASU pre-neural handoff, while the
  # fsr1-best-findings-temporal arm uses that handoff plus the combined settings.
  # Lanczos is the matched
  # spatial control generated by run_quality.sh from the exact same captured
  # native source pixels; selecting it here prevents a Temporal Forge image
  # from being mislabeled as the spatial baseline.
  if [[ "$arm_id" == "native-input" ]]; then
    # Native input is the exact GPU-decoded source captured by run_quality.sh.
    # It remains physically at input resolution; the filename's output token
    # records the comparison canvas requested by the reviewer.
    source_png="$(find "$run_dir/artifacts/quality_frames" -maxdepth 1 -type f -name '*_gpu_raw.png' -print -quit)"
  elif [[ "$arm_id" == "native-output" || "$arm_id" == "native-reference" ]]; then
    # Native reference is the lossless corpus reference rendered at the
    # selected output size. It is the same target used by quality metrics.
    source_png="$(find "$run_dir/artifacts/quality_frames" -maxdepth 1 -type f -name "${scene}_reference_${output_res}_f*.png" -print -quit)"
  elif [[ "$arm_id" == "lanczos" ]]; then
    source_png="$(find "$run_dir/artifacts/quality_frames" -maxdepth 1 -type f -name '*_lanczos.png' ! -name '*gpu_raw*' -print -quit)"
  elif [[ "$arm_id" == "pre-campaign-temporal-forge" ]]; then
    source_png="$(find "$run_dir/artifacts/quality_frames" -maxdepth 1 -type f -name '*_pre-campaign-temporal-forge.png' ! -name '*gpu_raw*' -print -quit)"
  else
    source_png="$(find "$run_dir/artifacts/quality_frames" -maxdepth 1 -type f -name "*_${arm_id}.png" ! -name '*gpu_raw*' -print -quit)"
  fi
  [[ -s "$source_png" ]] || { printf 'no captured image for %s\n' "$safe" >&2; return 1; }
  case "$arm_id" in
    lanczos|pre-campaign-temporal-forge|fsr1|best-findings-temporal|fsr1-best-findings-temporal|best-findings-temporal-synthetic-jitter)
      [[ "$(basename "$source_png")" == *"_${arm_id}.png" ]] || {
        printf 'source provenance mismatch for %s: %s\n' "$safe" "$source_png" >&2
        return 1
      }
      ;;
  esac
  cp -- "$source_png" "$final_png"
}

mapfile -t arms < <(jq -r '.arms[].id' "$manifest")
if [[ -n "$arm_filter" ]]; then
  arms=("$arm_filter")
  jq -e --arg id "$arm_filter" '.arms[] | select(.id==$id)' "$manifest" >/dev/null || {
    printf 'unknown review arm: %s\n' "$arm_filter" >&2
    exit 2
  }
fi
tasks=()
for scene in "${scenes[@]}"; do
  for input_res in "${inputs[@]}"; do
    for arm in "${arms[@]}"; do
      # Native input has no output dimension. Native output has no input
      # dimension and is captured once per scene/output. All actual scaling
      # methods retain the complete input/output Cartesian matrix.
      if [[ "$arm" == "native-input" ]]; then
        tasks+=("$arm $scene $input_res")
      elif [[ "$arm" == "native-output" ]]; then
        [[ "$input_res" == "${inputs[0]}" ]] || continue
        for output_res in "${outputs[@]}"; do tasks+=("$arm $scene - $output_res"); done
      else
        for output_res in "${outputs[@]}"; do tasks+=("$arm $scene $input_res $output_res"); done
      fi
    done
  done
done

printf 'review matrix: %d captures, %d workers\n' "${#tasks[@]}" "$workers"
active=(); failed=0
for task in "${tasks[@]}"; do
  read -r arm scene input_res output_res <<< "$task"
  [[ "$input_res" == "-" ]] && input_res=""
  run_task "$arm" "$scene" "$input_res" "$output_res" &
  active+=("$!")
  if (( ${#active[@]} >= workers )); then
    wait "${active[0]}" || failed=1
    active=("${active[@]:1}")
  fi
done
for pid in "${active[@]}"; do wait "$pid" || failed=1; done
(( failed == 0 )) || { printf 'review matrix had failures; artifacts retained at %s\n' "$campaign_root" >&2; exit 1; }
printf 'review matrix complete: %s\n' "$review_root"
