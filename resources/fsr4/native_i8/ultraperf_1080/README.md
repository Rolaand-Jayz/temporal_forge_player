# Native INT8 FSR4 ultraperformance 1080p assets

These Vulkan SPIR-V modules are compiled from the generated 1080p
`fsr4_model_v07_i8_ultraperf` HLSL in
`Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`.

Compilation uses DXC SPIR-V targeting Vulkan 1.2, shader model 6.6, `-O3`,
native FP16/INT8, and the same fixed storage-buffer bindings as the 2160p
pack. The graph uses a 20,736,000-byte scratch buffer and shares the
byte-identical initializer data stored with the 2160p pack.
