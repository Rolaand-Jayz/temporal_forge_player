# Native INT8 FSR4 ultraperformance assets

These Vulkan SPIR-V modules are compiled from the generated 2160p
`fsr4_model_v07_i8_ultraperf` HLSL in
`Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`.

Compilation uses DXC SPIR-V targeting Vulkan 1.2, shader model 6.6, `-O3`,
native FP16/INT8, and fixed storage-buffer bindings:

- `0`: model input, FP16 NHWC with 8 physical lanes and 7 logical channels
- `1`: INT8 initializer data
- `2`: model output, FP16 NHWC with 8 channels
- `3`: 82,944,000-byte scratch buffer

The generated source is copyright Advanced Micro Devices, Inc. These binary
assets preserve that implementation and are not hand-written substitutes.
