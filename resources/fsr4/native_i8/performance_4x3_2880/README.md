# Native INT8 FSR4 Performance 4:3 2880x2160 assets

Derived from the RE repository's generated 3840x2160 Performance HLSL using
`tools/adapt_native_int8_hlsl.sh`; only spatial tensor dimensions and NHWC
byte strides are adapted. The 89,216-byte initializer remains paired with
this graph. All 14 Vulkan modules are compiled and validated by
`tools/build_native_int8_pack.sh`.
