# Native INT8 FSR4 performance 2160p assets

These Vulkan SPIR-V modules and initializer are compiled from the generated
2160p `fsr4_model_v07_i8_performance` model in
`Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`.

Compilation uses DXC SPIR-V targeting Vulkan 1.2, shader model 6.6, `-O3`,
native FP16/INT8, fixed storage-buffer bindings, and an 82,944,000-byte scratch
buffer. The Performance initializer is distinct from the Ultra Performance
initializer and must remain paired with this shader pack.
