# Native INT8 FSR4 Quality 4:3 1440x1080 assets

Derived from the RE repository's generated 1920x1080 Quality HLSL using
`tools/adapt_native_int8_hlsl.sh`; only spatial tensor dimensions and NHWC
byte strides are adapted. The 89,216-byte initializer remains paired with
this graph. All 14 Vulkan modules are compiled and validated by
`tools/build_native_int8_pack.sh`.
