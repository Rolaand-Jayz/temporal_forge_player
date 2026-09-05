# FSR 4.1.0 RE Reconstruction — Status & Technical Log

> **Date**: 2026-07-09
> **Goal**: GPU-native FSR 4.1 temporal upscaling on Linux/Vulkan/AMD RDNA3
> via reverse-engineering the neural network from the extracted weight blob.
> The official FSR4 SDK ships as prebuilt D3D12/Windows DLLs — no native
> Linux/Vulkan backend exists.

---

## Architecture Overview

FSR 4 (`fsr4_model_v07_fp8_no_scale`) is a 14-pass U-Net CNN:

```
Input (7ch, H×W)
  ├── encoder1: DownscaleStridedConv2x2  7→16ch,  /2 spatial  [FP16 weight]
  ├── encoder2: ResBlock×2 (dw+pwx+pwc)  16ch     + Downscale  16→32ch, /2
  ├── encoder3: ResBlock×2 (sm+pwx+pwc)  32ch     + Downscale  32→64ch, /2
  ├── bottleneck: ResBlock×3 (sm+pwx+pwc) 64→128→64ch
  │   └── UpscaleConvTranspose2x2  64→32ch, ×2
  ├── decoder3: ResBlock×2 (sm+pwx+pwc)  32→64→32ch
  │   └── UpscaleConvTranspose2x2  32→16ch, ×2
  ├── decoder2: ResBlock×2 (dw+pwx+pwc)  16→32→16ch
  │   └── UpscaleConvTranspose2x2  16→8ch,  ×2
  └── Output (8ch, H×W) → postpass composites to RGB
```

Total: **39 conv steps** across 14 model passes. Spatial pyramid:
1.0× → 0.5× → 0.25× → 0.125× → 0.25× → 0.5× → 1.0×

Channel flow: 7 → 16 → 32 → 64 → 128 → 64 → 32 → 16 → 8

---

## Weight Blob Layout (131072 bytes)

| Zone | Offset | Size | Content |
|------|--------|------|---------|
| FP16 params | 0 | 7208 | 39 biases + 1 FP16 conv weight (encoder1 downscale) |
| FP8 weights | 7208 | 122880 | 38 quantized weight tensors (E4M3 codebook, 255 unique values) |
| Extra params | 130088 | 888 | v4.1.0-only zone; docs conflict between likely FP16 scale data and postpass FP32 params |
| Padding | 130976 | 96 | Zeros |

Candidate layout from `RE-of-FSR-4.1.0-Upscaling-1.0/spec/blob-format.json`
and `docs/extra-params-analysis.md`. The RE is not validated, so treat this
zone as unresolved until this project proves the postpass and FP8 decode path
locally.

---

## Verified Conv Step Table (from tensor-map.json)

All offsets are the exact per-tensor byte offsets in the 131072-byte blob.

| Step | Pass | Role | Weight Off | Bias Off | Kernel | Cin | Cout | Spatial | Notes |
|------|------|------|-----------|----------|--------|-----|------|---------|-------|
| 0 | 0 | dnsc | 0 | 1024 | 2×2 | 7 | 16 | /2 | FP16 weight (in bias zone) |
| 1 | 1 | dw | 7208 | 1088 | 3×3 | 16 | 16 | /2 | |
| 2 | 1 | pwx | 9512 | 1152 | 1×1 | 16 | 32 | /2 | |
| 3 | 1 | pwc | 10024 | 1280 | 1×1 | 32 | 16 | /2 | |
| 4 | 2 | dw | 10536 | 1344 | 3×3 | 16 | 16 | /2 | |
| 5 | 2 | pwx | 12840 | 1408 | 1×1 | 16 | 32 | /2 | |
| 6 | 2 | pwc | 13352 | 1536 | 1×1 | 32 | 16 | /2 | |
| 7 | 3 | dnsc | 13864 | 1600 | 2×2 | 16 | 32 | /2→/4 | |
| 8 | 4 | dw | 15912 | 1728 | 3×3 | 16 | 16 | /4 | spatial_mixing |
| 9 | 4 | pwx | 18216 | 1792 | 1×1 | 32 | 64 | /4 | |
| 10 | 4 | pwc | 20264 | 2048 | 1×1 | 64 | 32 | /4 | |
| 11 | 5 | dw | 22312 | 2176 | 3×3 | 16 | 16 | /4 | |
| 12 | 5 | pwx | 24616 | 2240 | 1×1 | 32 | 64 | /4 | |
| 13 | 5 | pwc | 26664 | 2496 | 1×1 | 64 | 32 | /4 | |
| 14 | 6 | dnsc | 28712 | 2624 | 2×2 | 32 | 64 | /4→/8 | |
| 15 | 7 | dw | 36904 | 2880 | 3×3 | 16 | 32 | /8 | |
| 16 | 7 | pwx | 41512 | 3008 | 1×1 | 64 | 128 | /8 | |
| 17 | 7 | pwc | 49704 | 3520 | 1×1 | 128 | 64 | /8 | |
| 18 | 8 | dw | 57896 | 3776 | 3×3 | 16 | 32 | /8 | |
| 19 | 8 | pwx | 62504 | 3904 | 1×1 | 64 | 128 | /8 | |
| 20 | 8 | pwc | 70696 | 4416 | 1×1 | 128 | 64 | /8 | |
| 21 | 9 | dw | 78888 | 4672 | 3×3 | 16 | 32 | /8 | |
| 22 | 9 | pwx | 83496 | 4800 | 1×1 | 64 | 128 | /8 | |
| 23 | 9 | pwc | 91688 | 5312 | 1×1 | 128 | 64 | /8 | |
| 24 | 9 | upsc | 119336 | 5568 | 2×2 | 64 | 32 | /8→/4 | HWCN layout |
| 25 | 10 | dw | 99880 | 5696 | 3×3 | 16 | 16 | /4 | |
| 26 | 10 | pwx | 102184 | 5760 | 1×1 | 32 | 64 | /4 | |
| 27 | 10 | pwc | 104232 | 6016 | 1×1 | 64 | 32 | /4 | |
| 28 | 11 | dw | 106280 | 6144 | 3×3 | 16 | 16 | /4 | |
| 29 | 11 | pwx | 108584 | 6208 | 1×1 | 32 | 64 | /4 | |
| 30 | 11 | pwc | 110632 | 6464 | 1×1 | 64 | 32 | /4 | |
| 31 | 11 | upsc | 127528 | 6592 | 2×2 | 32 | 16 | /4→/2 | HWCN layout |
| 32 | 12 | dw | 112680 | 6656 | 3×3 | 16 | 16 | /2 | |
| 33 | 12 | pwx | 114984 | 6720 | 1×1 | 16 | 32 | /2 | |
| 34 | 12 | pwc | 115496 | 6848 | 1×1 | 32 | 16 | /2 | |
| 35 | 13 | dw | 116008 | 6912 | 3×3 | 16 | 16 | /2 | |
| 36 | 13 | pwx | 118312 | 6976 | 1×1 | 16 | 32 | /2 | |
| 37 | 13 | pwc | 118824 | 7104 | 1×1 | 32 | 16 | /2 | |
| 38 | 13 | upsc | 129576 | 7168 | 2×2 | 16 | 8 | /2→/1 | HWCN, final output |

Candidate source: `RE-of-FSR-4.1.0-Upscaling-1.0/spec/tensor-map.json`
(78 tensors). This table is an implementation hypothesis, not an authority.

---

## What Works (Verified via GPU Diagnostics)

### 1. Prepass ✅
Extracts linear-light RGB features from the source frame. Writes 7-channel
FP32 features to scratch slot 0 in `[y][x][ch]` layout.

**Verified**: `scratch[0..256] nz=59 range[0,1] f8=[0.170, 8.8e-06, 0.935, 0,0,0,0, 0.167]`

### 2. Conv Step 0 (encoder1 downscale) ✅
Standard 2×2 strided conv, 7→16 channels, FP16 weights at blob offset 0.

**Verified spatial variation** (16ch output at 320×180):
```
ch  0: p0=0.000  p50=0.451  p100=0.000  p200=0.000  VARY
ch  2: p0=2.698  p50=1.986  p100=2.513  p200=2.750  VARY
ch  4: p0=1.742  p50=2.036  p100=1.707  p200=1.718  VARY
ch  8: p0=0.905  p50=1.165  p100=0.047  p200=0.751  VARY
ch 12: p0=0.066  p50=0.000  p100=0.000  p200=0.079  VARY
```

### 3. Buffer Pipeline ✅
- Submit / memory mapping / offset arithmetic: verified via vkCmdFillBuffer marker
- Ping-pong double-buffer: 39 steps → final output from step 38 in slot 1 at byte
  offset `slotSize * 4`. `finalAccumOffsetBytes_` tracks this correctly.
- Scatter (accum→scratch copy): verified matching conv output.

### 4. E4M3 FP8 Dequant ⚠️
The shader currently implements direct IEEE E4M3 FN reconstruction
(1 sign + 4 exp bias 7 + 3 mantissa).
Verified: byte 96 → 32.0, byte 16 → 0.03125, byte 61 → 1.625.
This proves the local decoder does what it says; it does not prove AMD's FSR4
runtime interprets the bytes that way.

### 5. FP16 Weight Dequant ✅
Pass 0's FP16 weights (only weight in the bias zone) correctly decoded via
`unpackHalf2x16`. Range [-0.925, 1.035], mean -0.006.

### 6. Full 39-Step Conv Chain ✅ (runs without GPU errors)
All 39 steps execute, producing non-zero output. The accum readback shows
non-zero values: `f8=[2,0,512,0.953,2,1.231,0,0.904]`.

---

## Current Status: Structural Proof Restored, Visual Validation Pending

The tensor-map offsets and FP16 bias interpretation are now aligned with the
recovered `tensor-map.json`/blob zones. The first non-finite tensor was then
localized with capped GPU dispatches: operation 34's depthwise output was
written directly to binary16 even though it crossed an FP8 boundary. The
depthwise and scalar pointwise paths now apply the recovered finite FP8
CopySat boundary; the cooperative pointwise path uses the same boundary.

The final transpose is intentionally retained as FP16 for the postpass. Its
conversion is explicitly saturated to binary16's finite range so synthetic
proof inputs cannot store `+/-Inf` and poison later stages.

Current live RDNA3 synthetic proof on the RX 7900 GRE:

```text
Stage 1: dispatch execution passed
Stage 2: final accum tensor passed sanity
Range: [-65504, 65504]
NaN/Inf: 0
Verdict: FSR4_INT8_PROOF_PASSED
```

This is a structural gate only. It does not establish parity with AMD's
private runtime, image quality, or production frame pacing. The direct E4M3
decoder and final saturation remain local reconstruction choices until a
trusted stage-3 reference is available.

### Measured GPU Optimization

On the RX 7900 GRE / RADV NAVI31, the harness now selects the reported
`FLOAT16 × FLOAT16 → FLOAT32` cooperative-matrix mode by default. FP8 model
weights are decoded once during upload and stored in a derived FP16 region;
the raw blob and validated INT8/scales representation remain available. Set
`TFORGE_FSR4_DISABLE_FP16_COOP=1` to compare or fall back to the INT8 path.

Repeated synthetic conv-chain timings were:

| Path | Runs | GPU time |
| --- | --- | ---: |
| FP16 cooperative (default on this GPU) | 41.87, 42.03, 42.02 ms | ~42.0 ms |
| INT8 cooperative fallback | 57.36, 57.76, 57.86 ms | ~57.7 ms |

This is approximately a 27% reduction in synthetic conv-chain time, but it is
still above the 16.67 ms 60 FPS budget and does not include a visual A/B or
full playback frame-pacing measurement. It must not be described as official
FSR4 parity.

### Current Hypotheses

The remaining parity questions are not resolved by the structural proof. The
unvalidated RE material suggests the real FSR4 shaders may use:

1. **Integer cooperative-matrix MAC** (VK_KHR_cooperative_matrix / WMMA)
2. **256-entry LUT-based FP8→FP16 decode** via `atomicCompareExchange` as a
   side-effect-free table lookup. Each FP8 byte produces pre-scaled FP16
   values — the LUT encodes both the E4M3 dequant AND per-tensor scaling
   in a single lookup.
3. The integer accumulation pipeline has different rounding/precision than
   float-domain MAC.

Our reconstruction currently uses:

1. **FP16 cooperative-matrix MAC by default on devices reporting the verified
   FP16 fallback mode**, with the INT8 cooperative path retained as an explicit
   fallback
2. **Direct E4M3 dequant at weight-upload time** into a derived FP16 region
   (the INT8 path retains its packed codepoints and scales)
3. **Float32 accumulation followed by the recovered finite FP8 boundary**

The FP16 fallback is a performance and numerical-fidelity experiment for this
reconstruction, not a claim about the private AMD implementation.

These remain implementation hypotheses rather than claims about AMD's private
runtime. Visual A/B testing against representative decoded video is still
required before enabling this path as a default-quality backend.

### Verified Diagnostic History

`TFORGE_FSR4_MAX_STEPS=N ./build/tests/fsr4_harness_tests` localized the first
non-finite tensor after the tensor-map and bias fixes:

| Step cap | Last step | Tensor | Result |
| --- | --- | --- | --- |
| 34 | step 33 | decoder2 pointwise expand, weight 114984 | finite |
| 35 | step 34 | decoder2 depthwise, weight 115496 | non-finite before boundary fix |
| 36 | step 35 | decoder2 depthwise, weight 116008 | finite after downstream overwrite, not a safe diagnostic boundary |
| 39 | step 38 | final 16→8 transpose | finite after depthwise FP8 boundary and final FP16 saturation |

The key fix was not an arbitrary global scale: learned intermediate outputs
were being written straight to binary16 even though the recovered graph uses
finite FP8 boundaries. The dead `quantizeFp8()` helpers are now used by the
depthwise, scalar pointwise, and cooperative pointwise output paths.

Partial spatial-mixing handling has been corrected locally: those passes now
mix only their weighted subset but preserve the full tensor width expected by
the following pointwise pass. The encoder2/encoder3 downscale dispatch grid is
also kept at `/4` and `/8` output resolution respectively.

The conv uniform block is 64-byte padded, and the constant-buffer ring removes
per-pass uniform allocation. The synthetic proof still remains fail-closed
when diagnostic overrides are active.

The synthetic proof path and real-frame dispatch now share one local 39-step
table and the same config builder, so capped diagnostics and real dispatch no
longer risk drifting apart when tensor geometry is corrected.

The project does not define completion as numerical comparison against AMD's
Windows runtime. There is no official FSR video reference to compare against;
completion requires local GPU execution, finite output, and visual A/B review.

---

## Shader Architecture

### conv_dw_dot4.comp (depthwise / spatial conv)
- Standard 2D convolution with inner loop over CIN input channels
- Weight layout: HWNC = [KH][KW][Cin][Cout]
- Weight index: `weightBase + (ky*KW + kx) * CIN * COUT + ic * COUT + oc`
- Supports stride-2 downscale (bit 31 of slot0.w)
- Supports FP16 weights (bit 30 of slot0.w)
- ReLU activation is currently implemented as `max(x, 0.0)` based on the RE
  notes, but this is still subject to local validation.

### conv_pw_dot4.comp (pointwise 1×1 conv)
- Standard 1×1 conv (channel mixing)
- Dispatches per-pixel, loops over (ic, oc)
- No activation (passes 3, 6 per activation-lut-analysis.md)

### CB Layout (shared by dw + pw)
```
slot0 = (featBase_words, accumBase_words, weightBase_bytes, cin|flags)
slot1 = (dispatchW, dispatchH, kernelSize, cout|biasPacked)
slot2 = (weightChannelsIn, weightChannelsOut, fp8ScaleBits, reserved)
```
Flags in slot0.w: bit 31 = stride-2 downscale, bit 30 = FP16 weights.
Cout in slot1.w low 16 bits, bias byte offset in high 16 bits.

### Ping-Pong Double Buffer
- scratch and accum each have 2 slots of `128 * srcW * srcH` FP32 words
- Step N (passCounter=N before increment):
  - featBase = (N % 2) * slotSize
  - accumBase = ((N+1) % 2) * slotSize
- 39 steps → final output from step 38 in slot 1

---

## Diagnostic Methodology

The harness includes a split-submit diagnostic (`diagnosePrepass`) that:
1. Runs prepass alone → reads scratch → confirms features written
2. Runs prepass + conv0 → reads accum → confirms conv produces output
3. Compares per-pixel values to check spatial variation

This isolates failures layer-by-layer since the main dispatch submits all
39 steps atomically (can't read back mid-dispatch).

---

## RE Data Sources

All imported hypotheses from: `RE-of-FSR-4.1.0-Upscaling-1.0/`

| File | Purpose |
|------|---------|
| `spec/tensor-map.json` | Candidate 78-tensor offset table |
| `spec/blob-format.json` | Zone layout + DLL RVA locations |
| `reports/v410_independent_offsets.json` | LLVM IR rawBufferLoad offset trace |
| `docs/architecture.md` | Network topology (14 passes, U-Net) |
| `docs/shader-internals.md` | LUT decode mechanism, pass structure |
| `docs/activation-lut-analysis.md` | Candidate activation notes |
| `docs/extra-params-analysis.md` | Candidate interpretation of extra zone |
| `extracted/v410_initializers/quality.bin` | 131072-byte weight blob |

---

## Next Steps

1. **Stage-3 validation**: Capture or generate a trusted reference output so
   the fan-in-scaled Vulkan path can be checked for local output behavior and
   visual quality.
2. **Verify scatter correctness**: Ensure the accum→scratch copy preserves
   spatial data between layers (check for stride mismatch).
3. **Try without ReLU**: Temporarily disable ReLU to see if signal survives
   (the LUT-based activation may differ from plain ReLU in edge cases).
4. **Check weight magnitudes**: Compare E4M3 dequant output distribution
   against expected weight statistics for trained networks.
5. **Postpass restoration**: Fix the descriptor layout GPU error to enable
   the postpass composite (needed for real RGB output from 8 features).
