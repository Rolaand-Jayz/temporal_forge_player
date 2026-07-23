# Native INT8 FSR4 Quality 2160p assets

These Vulkan SPIR-V modules and initializer are compiled from the generated
2160p `fsr4_model_v07_i8_quality` model in
`Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`.

Compilation targets Vulkan 1.2 and shader model 6.6 with native FP16/INT8. The
Quality initializer must remain paired with this shader pack. The fixed graph
uses an 82,944,000-byte scratch buffer.
