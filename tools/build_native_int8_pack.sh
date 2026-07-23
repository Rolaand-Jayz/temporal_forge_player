#!/usr/bin/env bash
set -euo pipefail

if (( $# != 3 )); then
    printf 'usage: %s PASSES_HLSL INITIALIZERS_BIN OUTPUT_DIR\n' "$0" >&2
    exit 2
fi

source_hlsl="$(realpath "$1")"
initializer="$(realpath "$2")"
output_dir="$3"
runtime_dir="$(dirname "$(dirname "$(dirname "$(dirname "$source_hlsl")")")")/dx12"
workgroup_overrides="$output_dir/workgroup_overrides.txt"

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
printf 'native INT8 pack ready: %s\n' "$output_dir"
