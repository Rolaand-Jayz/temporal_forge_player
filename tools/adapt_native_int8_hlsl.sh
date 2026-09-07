#!/usr/bin/env bash
set -euo pipefail

if (( $# != 6 )); then
    printf 'usage: %s SOURCE_HLSL OUTPUT_HLSL SOURCE_WIDTH SOURCE_HEIGHT TARGET_WIDTH TARGET_HEIGHT\n' "$0" >&2
    exit 2
fi

source_hlsl="$(realpath "$1")"
output_hlsl="$2"
source_width="$3"
source_height="$4"
target_width="$5"
target_height="$6"

for dimension in "$source_width" "$source_height" "$target_width" "$target_height"; do
    [[ "$dimension" =~ ^[1-9][0-9]*$ ]] || {
        printf 'dimensions must be positive integers\n' >&2
        exit 2
    }
done
[[ -s "$source_hlsl" ]] || {
    printf 'source HLSL does not exist: %s\n' "$source_hlsl" >&2
    exit 1
}

# ML2Code emits the output tensor as (width,height,7). Do not adapt an
# unrelated graph or silently change a model with a different spatial layout.
grep -Eq "uint3\\(${source_width},[[:space:]]*${source_height},[[:space:]]*7\\)" "$source_hlsl" || {
    printf 'source does not declare the expected %sx%s output tensor\n' \
        "$source_width" "$source_height" >&2
    exit 1
}

for level in 0 1 2 3; do
    divisor=$((1 << level))
    (( source_width % divisor == 0 && source_height % divisor == 0 &&
       target_width % divisor == 0 && target_height % divisor == 0 )) || {
        printf 'dimensions are not integral at stage 2^%d\n' "$level" >&2
        exit 1
    }
done

mkdir -p "$(dirname "$output_hlsl")"
cp "$source_hlsl" "$output_hlsl"

# The generated graph contains four spatial scales. Replace exact dimensions
# from coarse to fine so substitutions cannot overlap.
for level in 0 1 2 3; do
    divisor=$((1 << (3 - level)))
    old_width=$((source_width / divisor))
    new_width=$((target_width / divisor))
    old_height=$((source_height / divisor))
    new_height=$((target_height / divisor))
    if (( old_width != new_width )); then
        perl -0pi -e "s/\\b${old_width}\\b/${new_width}/g" "$output_hlsl"
    fi
    if (( old_height != new_height )); then
        perl -0pi -e "s/\\b${old_height}\\b/${new_height}/g" "$output_hlsl"
    fi
done

# NHWC byte strides are width-derived in this generated family.
for level in 0 1; do
    divisor=$((1 << level))
    old_stride=$((source_width / divisor * 16))
    new_stride=$((target_width / divisor * 16))
    if (( old_stride != new_stride )); then
        perl -0pi -e "s/\\b${old_stride}\\b/${new_stride}/g" "$output_hlsl"
    fi
done

grep -Eq "uint3\\(${target_width},[[:space:]]*${target_height},[[:space:]]*7\\)" "$output_hlsl" || {
    printf 'adapted HLSL does not declare the requested %sx%s output tensor\n' \
        "$target_width" "$target_height" >&2
    exit 1
}
printf 'adapted native INT8 HLSL: %sx%s -> %sx%s: %s\n' \
    "$source_width" "$source_height" "$target_width" "$target_height" \
    "$output_hlsl"
