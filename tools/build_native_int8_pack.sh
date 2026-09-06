#!/usr/bin/env bash
set -euo pipefail

if (( $# != 3 && $# != 5 )); then
    printf 'usage: %s PASSES_HLSL INITIALIZERS_BIN OUTPUT_DIR [TARGET_WIDTH TARGET_HEIGHT]\n' "$0" >&2
    exit 2
fi

source_hlsl="$(realpath "$1")"
initializer="$(realpath "$2")"
output_dir="$3"
source_dir="$(dirname "$source_hlsl")"
# Generated packs in this repository keep the ML2Code runtime beside the
# generated HLSL under .build/dx12. Retain the older repository-level lookup
# for source trees that use the original layout.
if [[ -n "${TFORGE_ML2CODE_RUNTIME_DIR:-}" ]]; then
    runtime_dir="$(realpath "$TFORGE_ML2CODE_RUNTIME_DIR")"
elif [[ -d "$source_dir/dx12/ml2code_runtime" ]]; then
    runtime_dir="$source_dir/dx12"
else
    runtime_dir="$(dirname "$(dirname "$(dirname "$(dirname "$source_hlsl")")")")/dx12"
fi
workgroup_overrides="$output_dir/workgroup_overrides.txt"

if (( $# == 5 )); then
    target_width="$4"
    target_height="$5"
    [[ "$target_width" =~ ^[1-9][0-9]*$ && "$target_height" =~ ^[1-9][0-9]*$ ]] || {
        printf 'target dimensions must be positive integers\n' >&2
        exit 2
    }
    # A generated HLSL source must actually contain the requested base tensor
    # shape. This prevents fixed 16:9 binaries from being relabeled as 4:3
    # packs; callers must provide a source generated for the requested shape.
    if ! grep -Eq "uint3\\(${target_width},[[:space:]]*${target_height}," "$source_hlsl"; then
        printf 'source has no tensor shape %sx%s; refusing to relabel a different aspect ratio\n' \
            "$target_width" "$target_height" >&2
        exit 1
    fi
fi

for tool in dxc spirv-dis spirv-as spirv-val; do
    command -v "$tool" >/dev/null || {
        printf 'required tool not found: %s\n' "$tool" >&2
        exit 1
    }
done

if [[ ! -d "$runtime_dir/ml2code_runtime" ]]; then
    printf 'ML2Code runtime not found: %s\n' "$runtime_dir" >&2
    exit 1
fi
if [[ "$(stat -c %s "$initializer")" != 89216 ]]; then
    printf 'unexpected initializer size: %s\n' "$initializer" >&2
    exit 1
fi

mkdir -p "$output_dir"
cache_key_file="$output_dir/pack.sha256"
cache_key="$(
    {
        sha256sum "$source_hlsl" "$initializer" "$0"
        if [[ -f "$workgroup_overrides" ]]; then
            sha256sum "$workgroup_overrides"
        fi
        if (( $# == 5 )); then
            printf 'target=%sx%s\n' "$target_width" "$target_height"
        fi
    } | sha256sum | cut -d' ' -f1
)"
if [[ -s "$cache_key_file" && "$(<"$cache_key_file")" == "$cache_key" &&
      -s "$output_dir/initializers.bin" ]]; then
    complete=1
    for pass in {0..13}; do
        [[ -s "$output_dir/pass${pass}.spv" ]] || complete=0
    done
    if (( complete )); then
        printf 'native INT8 pack cache hit: %s\n' "$output_dir"
        exit 0
    fi
fi

mkdir -p "$output_dir/.build"
build_runtime_dir="$output_dir/.build/dx12"
mkdir -p "$build_runtime_dir/ml2code_runtime"
cp -a "$runtime_dir/ml2code_runtime/." \
    "$build_runtime_dir/ml2code_runtime/"
find "$build_runtime_dir/ml2code_runtime" -type f -name '*.hlsli' \
    -exec sed -i '/_Static_assert/d' {} +

for pass in {0..13}; do
    raw_spv="$output_dir/.build/pass${pass}.raw.spv"
    raw_asm="$output_dir/.build/pass${pass}.raw.spvasm"
    fixed_asm="$output_dir/.build/pass${pass}.spvasm"
    final_spv="$output_dir/pass${pass}.spv"

    dxc -spirv -fspv-target-env=vulkan1.2 -Vd \
        -T cs_6_6 -E "fsr4_model_v07_i8_pass${pass}" \
        -D "MLSR_PASS_${pass}" -O3 -enable-16bit-types -HV 2021 \
        -no-warnings -I "$build_runtime_dir" \
        -fvk-t-shift 0 0 -fvk-u-shift 2 0 \
        "$source_hlsl" -Fo "$raw_spv"
    spirv-dis "$raw_spv" -o "$raw_asm"

    # DXC emits invalid pointer OpCopyLogical aliases for HLSL resource
    # wrappers. Resource variables already have the required pointer type, so
    # remove each alias and replace its later uses with the original variable.
    awk '
        $3 == "OpCopyLogical" && $4 ~ /^%_ptr_StorageBuffer/ {
            alias[$1] = $5
            next
        }
        {
            for (i = 1; i <= NF; ++i)
                if ($i in alias) $i = alias[$i]
            print
        }
    ' "$raw_asm" > "$fixed_asm"

    override=""
    if [[ -f "$workgroup_overrides" ]]; then
        override="$(awk -v pass="$pass" '
            $1 == pass && NF == 4 { print $2, $3, $4; exit }
        ' "$workgroup_overrides")"
    fi
    if [[ -n "$override" ]]; then
        read -r local_x local_y local_z <<< "$override"
        entry="fsr4_model_v07_i8_pass${pass}"
        sed -Ei \
            "s|(OpExecutionMode %${entry} LocalSize) [0-9]+ [0-9]+ [0-9]+|\\1 ${local_x} ${local_y} ${local_z}|" \
            "$fixed_asm"
        grep -q "OpExecutionMode %${entry} LocalSize ${local_x} ${local_y} ${local_z}" \
            "$fixed_asm" || {
            printf 'failed to apply workgroup override for pass %d\n' "$pass" >&2
            exit 1
        }
    fi

    spirv-as --target-env vulkan1.2 "$fixed_asm" -o "$final_spv"
    spirv-val --target-env vulkan1.2 "$final_spv"
    printf 'validated pass %d\n' "$pass"
done

cp "$initializer" "$output_dir/initializers.bin"
printf '%s\n' "$cache_key" > "$cache_key_file"
printf 'native INT8 pack ready: %s\n' "$output_dir"
