# Native INT8 pack policy

The packs in this directory are fixed-shape generated graphs. Their tensor
metadata, storage strides, scratch sizes, and paired initializer blobs must be
treated as one artifact; they are not interchangeable or safe to relabel for
another aspect ratio.

The runtime selects these packs only for supported near-16:9 source tensors
and exact fixed output tiers. Arbitrary-aspect inputs, including 4:3, use the
generic FSR4 graph and its target-size resource cache.

The RE source repository also provides resolution-specialized generated HLSL.
For common 4:3 targets, `tools/adapt_native_int8_hlsl.sh` derives a guarded
specialization by changing only the graph's spatial tensor dimensions and
NHWC byte strides; `tools/build_native_int8_pack.sh` then compiles and caches
the 14 validated Vulkan modules. The paired initializer remains unchanged.
The compiler autodiscovers the RE ML2Code runtime, or accepts
`TFORGE_ML2CODE_RUNTIME_DIR` when the adapted HLSL is staged elsewhere. The
target-shape check and content-addressed cache remain correctness gates.

The current local cache contains Quality, Performance, and Ultra Performance
packs for both 1440x1080 and 2880x2160. The player selects the matching pack
from the requested multiplier and fixed 4:3 display target; unsupported sizes
continue through the generic arbitrary-aspect path.
