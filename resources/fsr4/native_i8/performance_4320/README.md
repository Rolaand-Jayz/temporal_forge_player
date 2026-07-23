# Native INT8 FSR4 Performance 4320p assets

These Vulkan SPIR-V modules and initializer are compiled from the generated
4320p `fsr4_model_v07_i8_performance` model in
`Rolaand-Jayz/fsr4-rdna3-optimization` at commit `49015b7`.

Compilation uses `tools/build_native_int8_pack.sh`, targets Vulkan 1.2 and
shader model 6.6, and keeps the Performance initializer paired with this fixed
7680x4320 graph. The graph requires a 331,776,000-byte scratch buffer.

`workgroup_overrides.txt` remaps passes 9 and 13 from 8x8 workgroups to
wave-sized 64x1 workgroups. The global invocation grids and shader arithmetic
are unchanged. On an RX 7900 GRE, three 180-frame 4K60 runs measured 16.693
ms/frame versus 16.746 ms/frame with 8x8. Same-frame 7680x4320 output was
byte-identical. The runtime applies these dispatch overrides only to this graph.
