# Native INT8 FSR4 NativeAA 2160p assets

These Vulkan SPIR-V modules and initializer are compiled from the generated
2160p `fsr4_model_v07_i8_native` model in
`Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`.

Compilation uses `tools/build_native_int8_pack.sh`, targets Vulkan 1.2 and
shader model 6.6, and keeps the NativeAA initializer paired with this fixed
3840x2160 graph. The graph requires an 82,944,000-byte scratch buffer.
