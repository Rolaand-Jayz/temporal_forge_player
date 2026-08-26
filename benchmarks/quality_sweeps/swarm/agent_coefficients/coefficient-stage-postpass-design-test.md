# Coefficient-stage / postpass design and test note

Status: evidence insufficient for implementation.

Scope: read-only reverse engineering of the current host and shader contracts. No shader, source, or default was changed. No GPU capture was run.

## Evidence inspected

- `shaders/fsr4/postpass_composite.comp`
- `src/render/Fsr4DispatchHarness.cpp`
- `src/render/Fsr4DispatchHarness.hpp`
- `tests/fsr4_postpass_contract_tests.cpp`
- `docs/FSR4_RE_STATUS.md`
- `docs/exec-plans/QUALITY_PERFECTION_M1_GATE.md`
- `docs/exec-plans/QUALITY_PERFECTION_EXECUTION_SLICES.md`
- `docs/slice-plan.md`
- `resources/fsr4/native_i8/README.md`
- `resources/fsr4/native_i8/ultraperf_2160/README.md`
- local generated native shader: `resources/fsr4/native_i8/performance_4320/.build/passes_filter4.hlsl`

No checked-in postpass/coefficient HLSL, IR, disassembly, or authoritative binding trace was found. The generated native HLSL is useful for the native tensor/register contract, but it does not identify the missing coefficient-stage/postpass operation or its semantic roles.

## Current generic postpass contract

`postpass_composite.comp` declares one descriptor set with these bindings:

| Binding | Type | Current role |
| --- | --- | --- |
| 0 | uniform buffer | `PostpassCB`, 128 bytes |
| 1 | read-only std430 storage buffer | raw/expanded generic weight blob |
| 2 | read-only std430 storage buffer | decoder output, `float16_t` channels |
| 3 | read-only `rgba16f` image | history compatibility binding; declared but not read |
| 4 | write-only `rgba8` image | composited output |
| 5 | write-only `rgba16f` image | history output |
| 6 | read-only `rgb10_a2` image | source color |
| 7 | read-only `rgba16f` image | reprojected color used by the temporal path |
| 8 | read-only `rg16f` image | motion compatibility binding; declared but not read |
| 9 | write-only `rgba16f` image | recurrent output |
| 10 | read-only `rgba8` image | source display cache |

The host creates the same 11-entry layout in `createPostpassPipeline()` and binds the generic postpass with:

- binding 0 at the constant-buffer ring offset `112 * 128 = 14,336` bytes;
- binding 1 at weight-buffer byte offset 0, whole buffer;
- binding 2 at decoder/final-tensor byte offset 0, whole buffer;
- bindings 3-10 to the image views listed above;
- dispatch dimensions `(outputWidth + 31) / 32` by `(outputHeight + 7) / 8`.

`PostpassCB.slot0.w` is the decoder base in shader word units and is currently written as zero. The shader indexes eight physical decoder channels per output pixel. The generic and native branches both currently expose the selected decoder output at byte offset zero, so this zero base is consistent with the known graph, but it is not a general coefficient-stage contract.

## Weight-tail address map

The shader constants and host validator agree on:

```text
blob size                 131,072 bytes
coefficient region        byte 130,088, 222 FP32 values, 888 bytes
coefficient region end    byte 130,976, exclusive
blob padding              byte 130,976 through 131,071
shader float index        130,088 / 4 = 32,522
coefficient indices       32,522 through 32,743, inclusive
output-bias group 0       byte 130,944, float indices 32,736..32,739
unresolved group 1        byte 130,960, float indices 32,740..32,743
```

Vulkan descriptor offsets and ranges are byte-based. GLSL indexing of `float weightParams[]` is element-based. A coefficient implementation must keep those units distinct. Passing `130,088` as a GLSL float index, or passing `32,736` as a Vulkan byte offset, is incorrect.

The current shader uses group 0 as an optional four-value decoder output bias when `slot0.z` bit 64 is set. The former local working-tree implementation also applied group 1 as a recurrent bias; that use was removed because no evidence established that semantic role. Group 1 must remain unresolved until a trace or equivalent RE evidence identifies its consumer.

`postpassParameterTrace()` reads all 222 values only to produce a finite checksum diagnostic. That proves a bounded load, not that every value has the correct coefficient-stage destination or contributes to the image result. `uploadWeights()` validates and uploads the raw blob, but `postpassParams_` is currently audit storage, not a separate shader coefficient buffer or host-side semantic map.

## Native fixed-pack contract

The native INT8 pipeline is a different resource graph. Its four storage-buffer bindings are:

| Native binding | Generated shader/register role | Host resource |
| --- | --- | --- |
| 0 | model input, `t0` | generic final-tensor input |
| 1 | INT8 initializer, `t1` | native initializer buffer |
| 2 | model output, `u0` | native INT8 output buffer |
| 3 | shared scratch, `u1` | native shared-scratch buffer |

The generated HLSL describes the final native output as FP16 NHWC with eight physical channels at byte offset zero. It also uses shader-encoded byte offsets for multiple tensors in the shared scratch allocation. Therefore:

- the generic raw weight-tail offset must not be assumed to be the native initializer offset;
- a coefficient stage reading or writing native scratch needs the same pack-specific byte offsets, strides, channel packing, and scratch-size contract as the generated native shader;
- a host descriptor offset of zero does not mean that every tensor begins at zero; the tensor offset may be encoded in the stage shader;
- generic and native resources must not be silently mixed merely because both eventually expose eight decoder channels to postpass.

The repository does not identify a coefficient-stage binding/register, native scratch tensor offset, coefficient element type, or coefficient stride. Those are implementation blockers, not safe assumptions.

## Concrete requirements before implementation

1. Obtain a source-of-truth map for the missing stage. For every load and store, record the resource binding/register, buffer byte base, element type, tensor shape, per-pixel stride, channel order, and destination contribution.
2. Identify whether coefficients come from the generic 222-value tail, the native initializer, native scratch, or another artifact. The current evidence does not establish this.
3. Decide whether the stage is a separate dispatch or part of `postpass_composite.comp`. The current host schedule contains the postpass dispatch but no named coefficient-stage dispatch.
4. If a new resource is required, add an explicit descriptor binding and host descriptor entry after the binding contract is known. Do not overload binding 1 or 2 without proving that the existing buffer type, range, and lifetime are identical.
5. Specify byte-versus-element units in the constant-buffer contract. `slot0.w` currently represents decoder words; it is not sufficient to express an arbitrary byte offset, tensor stride, or native scratch view.
6. Add a synchronization requirement from the coefficient-stage writer to the postpass reader. The current barriers cover the known output, history, recurrent, and reprojected images, but they do not prove visibility for a new coefficient buffer or scratch subrange.
7. Reserve a distinct constant-buffer slot if the stage needs different metadata. The current postpass slot is 112, residual slots occupy 96-106, and convolution offsets use the ring's first region. The host currently allocates `64 * 128 * 2 = 16,384` bytes; this is sufficient for the present slot 112, but the hard-coded reservation needs an explicit bounds/overlap test before adding stages.
8. Preserve the compatibility image bindings until the consuming shader contract is proven. In particular, the postpass currently consumes `u_reprojectedColor`; it does not establish that `u_history` or `u_motion` are direct coefficient-stage inputs.
9. Keep the unresolved second four-float group disabled or explicitly diagnostic. Do not infer recurrent-bias, exposure, sharpening, or other semantics from placement in the blob alone.

## Main risks

- Address-unit errors can read the wrong tail or padding while remaining in-bounds.
- A whole-buffer descriptor with a shader-side index can hide an incorrect base, while changing the descriptor offset without updating shader constants creates a second, independent offset error.
- The native output is FP16 with eight physical lanes, not an RGB byte image. Treating it as RGB, FP32, or a four-channel tensor can produce plausible-looking but invalid results or an out-of-bounds read.
- Native scratch is shared by multiple tensors and passes. An unproven offset can alias an intermediate tensor, and a missing barrier can make results dependent on scheduling.
- The generic tail is loaded through a raw float array, while native weights and initializer data have pack-specific representations. A matching byte count does not establish matching semantics.
- Constant-buffer slot 112 is safe for the current allocation but is not self-describing. Future pass growth can overlap the postpass metadata or exceed the ring unless the ranges are calculated and checked.
- Static contract tests and the checksum trace can pass even when a coefficient has no effect, affects the wrong channel, or is applied to the wrong stage.

## Proposed test plan

These tests are design requirements, not tests run in this sidecar.

### CPU and address-map tests

- Use a 131,072-byte synthetic blob with distinct little-endian sentinels at coefficient indices 0, 213, 214-221, and the first padding word.
- Assert the exact byte range, exclusive end, 888-byte size, and 32,522-32,743 float-index range.
- Assert group 0 maps only to byte 130,944-130,959 and group 1 only to byte 130,960-130,975.
- Reject short blobs, unaligned regions, non-finite values, and reads into the padding.
- Test conversions separately for Vulkan byte offsets, shader float indices, FP16 element indices, and native scratch byte offsets.

### Descriptor and host-contract tests

- Assert postpass bindings 0-10 have the declared descriptor types, image formats, and access modes.
- Assert generic binding 1 and binding 2 use the intended base/range and that `slot0.w` matches the selected decoder base.
- Assert native bindings 0-3 map to input, initializer, output, and scratch, with no generic-weight alias.
- Assert the constant-buffer ring is aligned and that convolution, residual, and postpass slot ranges do not overlap or exceed the allocation.
- Assert generic/native selection is deterministic across reset and recurrent-disabled paths.
- Add a visibility/barrier contract for every proposed coefficient-stage writer and postpass reader.

### Native-pack concordance tests

- Parse the checked-in native metadata and generated HLSL register map.
- Verify the paired initializer and native pack are selected together.
- Verify the output tensor's eight-lane FP16 layout and base offset.
- Verify every coefficient-stage scratch offset is within the pack's declared scratch size and has the expected alignment/stride.
- Fail if a proposed generic tail address is used as a native initializer or scratch address without explicit evidence.

### Semantic/reference tests

- Build a small CPU reference with synthetic decoder channels and coefficients where each candidate coefficient changes a different known output.
- Toggle one coefficient or coefficient group at a time and assert the expected channel/output delta.
- Assert group 1 has no production effect until its consumer is identified.
- Assert finite output, reset determinism, boundary pixels, and the exact eight-channel decoder stride.
- Distinguish a diagnostic checksum read from a real image-path contribution in the test naming and assertions.

### Later runtime gate

Only after the static and CPU gates pass should a Vulkan validation/readback test be added for the real dispatch sequence, descriptor ranges, barriers, and coefficient contribution. That runtime gate remains future work here; no GPU capture was performed.

## Recommendation

Do not implement the missing coefficient stage from the current repository evidence. First obtain one authoritative artifact or trace that resolves the stage's binding, source buffer, byte base, element type, stride, coefficient roles, and write destination. Then update the host/shader contract together and add the tests above before using any visual or quality result as evidence of correctness.
