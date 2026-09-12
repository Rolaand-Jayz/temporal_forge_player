# Executive Conclusion  
It **is technically possible** to feed pre-rendered video into FSR 4.1 by fabricating its temporal inputs, but only with significant limitations.  The FSR 4 API *requires* motion vectors and subpixel jitter offsets as inputs.  These do not exist in ordinary video, so we must estimate or fake them.  Extracting block‐based codec motion vectors or running an optical‐flow algorithm can produce candidates, and one can *simulate* camera jitter by shifting frames.  However, none of these fully match the original game‐renderer signals.  **Best motion‐vector strategy:** use a combination of decoded codec vectors (where available) refined with dense optical flow, while detecting and handling occlusions.  **Best jitter strategy:** in practice, use zero or minimal jitter; truly creating FSR‐style jitter *after* the fact cannot produce new spatial information from a finished frame.  The **biggest obstacle** is that video lacks the true per-pixel motion/jitter history that FSR expects, so many failure modes (disocclusions, flicker, misalignment) arise.  In sum, using FSR4 on video is *feasible* as a compatibility hack, but any quality gains are experimental: it may *run*, but making it outperform a strong video‐SR method is unlikely without major concessions.

# What FSR 4/4.1 Actually Requires  

FSR 4 (the ML‑based “Redstone” upscaler) has a well‐defined API.  In the official documentation, the required inputs (with conventions) are:

| **Input**              | **Format & Convention**                                                   | **Video-provided?**    | **Comments/Confidence**                          |
|------------------------|---------------------------------------------------------------------------|------------------------|--------------------------------------------------|
| Current **color**      | 2D color buffer in **linear** RGB (float); size = render resolution.  | ✓ (frames available)   | Video frames can be decoded and converted to linear if needed (FSR API can accept non-linear if flagged). |
| **Depth** (optional)   | 1× float (24-bit depth) at render resolution.               | ✗                     | No depth in video; can be omitted or set default (FSR has flags for missing/inverse depth). |
| **Motion vectors**     | 2-component float per pixel, encoding **current→previous** motion in pixel units.  Range: [–W..+W], [–H..+H].  Resolution = render resolution (unless a special flag for display res).  | ✗ (must fabricate)    | We must supply.  Video has none natively.  We can derive approximate MVs via flow or codec. |
| **Exposure** (optional) | 1×1 float, the scene’s pre-exposure or exposure (for auto-exposure). | Partial              | If video is gamma‐encoded, we supply frameTimeDelta and assume fixed exposure; can use default like 1.0.  Confidence low. |
| **Jitter offset**      | 2D subpixel offset (in pixels) used in projection this frame.  Passed in `ffxDispatchDescUpscale.jitterOffset`. | ✗ (must simulate) | Video frames have no true camera jitter.  We could set this to zero or a synthetic sequence. |
| **FrameTimeDelta**     | Scalar float (ms between frames).                              | ✓ (fps known)        | Easily derived from known frame rate or timestamps. |
| **Reset flag**         | Boolean (true on first frame or scene cut).                | ✗ (detectable)       | Not in video.  We must detect scene cuts and set `reset=true`.  Confidence: moderate if we use scene-detect heuristics. |
| *Reactive/Transparency masks* (optional) | Byte mask per pixel for blending (no longer required).      | ✗                  | Video has no such mask. They are *optional* and can be omitted. |

**FSR 4 expects:** color and motion vectors rendered **with jitter** in the projection matrix, except motion vectors themselves should *not* be jittered unless a cancellation flag is used.  In practice, we will supply an _unjittered_ color buffer (since we can’t really generate jitter), so we would set `jitterOffset` = (0,0) and disable any cancellation flags.  All inputs above marked “✗” must be synthesized or approximated.  We assign confidence levels as follows: color and frame time (✅ high), depth and exposure (✅ low – can default), motion vectors (❓ experimental – see below), jitter (❓ none or synthetic), reset (❓ scene-detect), masks (✅ omitted).  (Citations: AMD FSR docs.)

# Motion-Vector Research  

**What FSR expects:** Per-pixel 2D flow vectors (floats) mapping each *current* pixel’s position back to where that same point was in the *previous* frame.  In formula:  
```
PrevPos(x,y) = (x,y) + MV(x,y).
```  
Range is [–width..+width] horizontally and [–height..+height] vertically.  (E.g. MV=(W,H) at (0,0) means that pixel came from bottom-right.)  Internally FSR encodes these in 16-bit but we pass floats.  The **vector direction** is “current→previous” (backward flow).  Motion vectors should be in **screen space** (pixel units) with no projection/jitter transforms applied.  **Jitter note:** FSR docs say MV should be computed on *unjittered* coordinates unless using the jitter-cancellation flag.  So if we shift frames for jitter, MVs must remain in the original pixel coordinate system.  

**Sources of vectors and their limitations:**  
- **Codec motion vectors:** Many video codecs (H.264/AVC, MPEG-4, etc.) store block-based motion vectors used for compression.  In principle these can be extracted (FFmpeg example tools like extract_mvs or mv-extractor), but they differ from game-renderer MVs.  They are block‑aligned (e.g. 16×16 pixels), integer/quarter‐pixel in resolution, and biased toward compression efficiency rather than true optical flow.  Crucially, skipped or intra-coded blocks generate (0,0) vectors that may mean “no info” rather than truly static.  Standalone H.264 exporters give MV per 4×4 or 8×8 block with integer entries; HEVC/AV1 have variable block sizes (AV1 4×4 to 128×128) and fractional precision up to 1/8 pixel.  After extraction, one can upsample/interpolate these to per-pixel.  However, as Stefan Karlsson notes, codec MVs “are not ‘true motion’; they aim for best block prediction, not accuracy”.  They may be usable as an initial guess or for large homogeneous motion, but alone they’ll produce many errors (especially at boundaries or new objects).  In short, codec MVs *can* feed FSR but must be filtered/filled.  

- **Dense optical flow:** Classic or CNN-based optical-flow algorithms compute a 2D motion vector for every pixel, often with subpixel precision.  Options include Farneback or Lucas-Kanade pyramidal flows (e.g. OpenCV GPU), or learned models (FlowNet, RAFT, TeCFlow, etc.).  These can yield fine-grained MV fields directly matching FSR’s per-pixel format (after converting direction sign).  However, real-time constraints are severe.  Traditional GPU-accelerated approaches (e.g. NVIDIA’s Optical Flow SDK on Turing/Ampere/Ada GPUs) can compute forward/back flows in a few milliseconds for 1080p, but this requires NVIDIA hardware.  On AMD GPUs, no dedicated hardware exists; one must use general compute (e.g. OpenCL/Vulkan kernels) or run a neural model in DirectML/Vulkan.  Lightweight learning models (RAFT-lite variants) claim close to real-time performance after pruning/quantization, but generally these flows cost tens of milliseconds per frame on 1080p.  

- **Neural or hybrid methods:** Some approaches train video-SR models that implicitly compute motion (e.g. VESPCN, FRVSR) or use internal flows.  One could re-purpose a video-interpolation network (e.g. NVIDIA’s DLSS Frame Generation or FLowNet variants) to get motion, but extracting the flow from such black-box nets is nontrivial.  Combining coarse codec MVs with a refinement network is conceivable, but unproven for real-time.  Frame interpolation models (like DAIN, RIFE) inherently compute forward/back flows internally to generate interpolated frames; these flows are usually high-quality but are embedded in proprietary pipelines.  As an engineering approximation, one could run a fast frame interpolation model on consecutive frames to get fine flows, then use those flows for FSR.  

- **Block matching or hierarchical flow:** Simpler block-matching algorithms can be GPU-parallelized.  For example, a sliding-window search or phased correlation could yield full-frame MV fields at moderate cost.  Pyramidal (coarse-to-fine) optical flow (like Farneback) is another option, trading off precision for speed by using image pyramids.  These are easier to implement but generally less accurate on fast motion or large displacements.  

**Converting to FSR format:** If you obtain a flow field f(x,y) defined as “previous→current” (the usual “forward” flow), then the needed FSR vector (current→previous) is simply `MV = –f`.  For example, if a dense flow gives displacement (dx,dy) meaning point at (x,y) in frame N came from (x–dx, y–dy) in frame N–1, then FSR wants MV=(–dx, –dy) at (x,y).  If codec MVs are per-block (with block centers or corners), one can interpolate them to each pixel and scale by block size as needed.  AMD’s docs say you can adjust MVs via a `motionVectorScale` if your range differs.  **Example conversion:** suppose a codec block MV of (8,4) at a 16×16 block means each 8×8 macro pixel moved by that; you could assign MV= (8,4) to all pixels in that block.  The sign convention in AMD’s API is just as above.  

**Occlusion/disocclusion:** Renderer MVs know about geometry, but estimated flows do not.  In practice: when an area of frame N has no correspondence in frame N–1 (newly revealed background), the flow there is undefined.  In **codec MVs**, intra/skip-blocks give (0,0) which is ambiguous.  In **optical flow**, occluded pixels often produce noisy or invalid vectors.  Strategies: we can detect occlusion by checking backward-then-forward consistency or by examining texture matching confidence.  Wherever occlusion is detected, FSR might prefer a zero vector (static assumption) or signal a history reset.  The FSR docs mention using the `reset` flag for abrupt changes (like scene cuts), but they say nothing about per-pixel invalid MVs.  A safe approach is to **zero** any doubtful vectors.  In video-SR research, pixels with undefined motion are often blended or inpainted.  For FSR, giving MV=(0,0) (meaning “assume pixel static”) is probably best.  

**Failure modes:** All approaches will struggle on (a) *occlusions/disocclusions*: new or disappeared regions have no valid MV; (b) *motion blur and transparency*: flow cannot handle blurred objects or ghosted transparencies; (c) *cuts/fades*: scene cuts break temporal coherence – we must detect cuts (e.g. by frame-difference threshold) and set `reset=true` to force FSR to discard history; (d) *low-texture or repeated patterns*: flow is ambiguous there; (e) *compression artifacts*: codec MVs on noisy blocks can be junk; (f) *very fast motion*: flow often fails.  Mitigation: use confidence masks (e.g. from flow APIs), filter outliers (e.g. median filter on MVs), and rely on FSR’s internal heuristics (it blends history conservatively if MVs seem wrong).  Summarizing, any MV derived from two video frames is inherently imperfect.  Thus, we should treat MV-based upscaling as *approximate*: use it to improve FSR’s temporal alignment when it’s reasonably confident, but expect artifacts in occluded or high-speed areas.  

# Jitter Research  

**What jitter is:** In games, *camera jitter* means adding a small subpixel offset to the projection each frame (often drawn from a low-discrepancy sequence).  This causes each frame to sample slightly different points on each pixel, which FSR (and TAA) use to reconstruct higher-frequency detail. FSR’s API reflects this: it provides `getJitterOffset`, which yields a (Δx,Δy) in pixel units that you incorporate into your projection matrix.  The engine applies that jitter, renders, and then tells FSR the offset via `dispatchDesc.jitterOffset`.  FSR then shifts the accumulated history accordingly.  Critically, **FSR expects that the input color frames were actually rendered with that jitter**.  

**Key parameters:** FSR uses a predetermined jitter sequence (often Halton or a Sobol set) of length depending on quality mode.  For example, Balanced mode uses 23 phases.  The jitter offsets are nonzero vectors and tile uniformly in the unit pixel.  There is a direct formula to convert pixel jitter to projection-space offset.  Normally, the jitter amplitude is ±0.5 pixels or so (to equally sample pixel area).  

**Finished-video vs true jitter:** Once a frame is fully rendered (as a conventional video frame), its pixels are fixed.  Simply shifting the entire image by fractional pixels (resampling) does not create *new information* – it only re-interpolates the existing sample grid.  Therefore, “applying jitter” *after* the fact cannot magically recover detail beyond what was captured.  Formally, if you have one low-res image, no spatial transform can add true high-frequency detail.  

**Possible synthetic methods:**  
- *Fractional resampling:* The simplest is to take each video frame and translate it by (Δx,Δy) < 1px using bilinear or Lanczos resampling.  This mimics a subpixel jitter physically.  But it does *not* add detail; it just shifts image content.  If you alternate different shifts (i.e. run FSR on an input stream that is jittered artificially), you are effectively telling FSR the camera moved, but the actual color frame has no new fine sample – so FSR’s algorithm can only blend the same base data.  You get an image with slight aliasing or blur, not new texture.  

- *Oversampling / super-sampling:* One could upsample the frame to a higher intermediate resolution (e.g. via a spatial super-resolution model or simple upsample) and then shift and downsample again, hoping to create pseudo-aliased differences.  For example, run a 2× upscaler on the 720p frame to 1440p, then shift by half a pixel, then downsample back to 720p.  This can produce *different* low-res samples, but only because you introduced an approximate HR guess.  Its effectiveness depends entirely on the upscaler quality.  

- *Using neighboring frames:* If objects move slightly between frames, adjacent frames effectively provide different “subpixel samples” of the scene.  One could use optical flow to warp neighbor frames onto the current frame’s grid, creating synthetic jittered versions.  For instance, warp frame N–1 forward to frame N with a subpixel offset.  This uses true temporal information, but it’s now a form of multi-frame super-resolution (and again limited by flow accuracy).  

- *No-jitter mode:* The FSR API can also work with zero jitter (essentially disabling TAA).  In that case, it still uses history via motion vectors, but doesn’t expect new sample points.  This may be the only realistic mode for video.  

**Meaningful vs trivial:** Only approaches that actually change *which input pixels* are sampled relative to the grid can introduce distinct information.  Pure translation of a fixed-sampled image does not.  Downsampling high-res data with different offsets (possible if we had a higher-res source) does create distinct low-res inputs; but in our use case, the video frame is the highest available.  Thus, **information-theoretic limit:** without an actual higher-res source or extra viewpoint, artificial jittering cannot yield new image content.  The best we can hope is that some temporal blending with nearby frames (which are genuinely different images) can **simulate** jittered samples, but that is not equivalent to the game’s underlying intent.  

**Recommendation:** Given this, the most practical “jitter” is simply **zero** or a trivial repeating offset.  We will run FSR in effectively zero-jitter mode (no subpixel input shifts).  This means FSR will rely entirely on motion compensation without new sample diversity.  (We cite AMD’s docs on jitter usage to explain what FSR expects, and note that post-hoc jitter resampling is not equivalent.)  

# Recommended Integrated Real-Time Architecture  

A feasible pipeline (on a modern GPU) is:  

1. **Video Decode:** Use a hardware-accelerated decoder (NVDEC on NVIDIA, AMD VCN, or DirectML on GPU) to decode each frame into a GPU texture.  Convert colors to linear space if needed for FSR.  

2. **Scene-cut Detection:** Compare the incoming frame with the previous one (e.g. via pixel difference or keyframe flag).  If a cut/fade is detected, set `reset=true` and skip motion/jitter steps for this frame, so FSR flushes history.  

3. **Motion Estimation:** Compute motion vectors from the last frame to the current frame.  Options (preferably GPU-accelerated):  
   - Try extracting codec MVs via FFmpeg (for H.264 it’s possible; H.265/AV1 likely not) as a fast first guess.  Convert block MVs to per-pixel as described above.  
   - Run a GPU optical-flow compute step on the decoded frames (e.g. a lightweight CNN like RAFT-small, or classic Farneback in a shader).  Compute **backward flow** (current→previous) directly if supported.  This is the heaviest step.  
   - Optionally combine: where block MVs exist, use them; otherwise fill in via interpolation of the dense flow.  Smooth/filter the MV field to remove outliers.  

4. **Occlusion/Masking (optional):** Based on the MV field, detect unreliable vectors (e.g. very large jumps or inconsistent forward/back consistency).  Generate a confidence or mask.  Mark those pixels’ MVs as zero if unsure.  

5. **Motion-Vector Conversion:** Scale and assign the MV data into the format expected by FSR: a GPU texture of 2×float per pixel encoding current→previous offset.  Ensure units match (e.g. if flow was in pixels, no extra scale needed).  If using a non-zero jitter scheme (not recommended), compensate here.  

6. **Jitter Simulation (optional):** Since we run no actual jitter, set `ffxDispatchDescUpscale.jitterOffset = (0,0)`.  (If we attempt any synthetic jitter, it would involve an extra image warp step here, which we skip.)  

7. **FSR 4 Upscaling:** Call the FSR Upscaling API with:  
   - *Inputs:* the current frame color (at chosen “render” resolution), the MV texture, and any other needed (e.g. an exposure factor if known).  Set `frameTimeDelta` to the frame interval in ms.  Set `reset` flag if needed.  
   - *Execution:* dispatch the FSR compute shader to produce the upscaled output (e.g. 4K).  

8. **Postprocessing:** Collect the upscaled frame.  Optional: apply any denoising/sharpening.  Send output to display or encode to video.  

**Buffers and Data Flow:**  
- Frame _N_ comes from video decoder → linearize → GPU texture A_N.  
- MVs: from flow on (A_N, A_{N-1}) → MV texture M_N.  
- FSR uses A_N (jittered=0) and M_N, plus history from N–1 (managed internally), to produce output O_N.  

Pseudocode outline:  
```
initialize FSR context, allocate textures;
prev_frame = empty;
for each decoded frame cur_frame {
  if (scene_cut(prev_frame, cur_frame)) dispatchDesc.reset = true;
  if (prev_frame exists) {
    MV = compute_motion(prev_frame, cur_frame);
  } else {
    MV = zero_field;
  }
  // Prepare inputs:
  convert cur_frame to linear texture;
  set dispatchDesc.color = cur_frame_texture;
  set dispatchDesc.motionVectors = MV_texture;
  dispatchDesc.jitterOffset = (0,0);
  dispatchDesc.frameTimeDelta = time_since_last_frame;
  ffxDispatch(dispatchDesc);
  output_frame = dispatchDesc.output;
  prev_frame = cur_frame;
}
```
(GPU operations can be pipelined for efficiency.)

# FSR 4/4.1 Integration Path  

On the software side, we must use AMD’s official FSR 4 API.  According to AMD’s SDK documentation, FSR 4.1.1 is delivered as **signed binary DLLs**.  Specifically, a host program loads `amd_fidelityfx_loader.dll` and then `amd_fidelityfx_upscaler.dll` to access FSR Upscaling 4.1 functionality.  This requires a supported GPU (AMD Radeon RX 7000/9000 Series or equivalent; on other hardware the loader will fall back to FSR 3.1).  

The typical API sequence is:  
- **Initialization:** Call `ffxCreateContext` or similar (via the loader) to create a FSR context, specifying Vulkan/D3D12 device, render API, and choosing the Upscaler technique.  Query required resources (history buffers, etc) using `ffxQueryGetResourceRequirements` (per the FSR docs).  
- **Frame dispatch:** For each frame, fill an `FfxFsrsUpscaleDispatchDesc` with the pointers to the current color texture, motion-vector texture, (and depth or exposure if used), plus the jitterOffset and frameTimeDelta fields.  Then call `ffxExecuteDispatch` (or similar) to run the upscaling shader.  This will write the upscaled output texture.  
- **Finalization:** Destroy the FSR context when done.  

In practice, one would link against AMD’s SDK headers to access the `ffx` API.  AMD provides a sample (in the FidelityFX SDK) showing how to set up FSR 4.1 (and 3.1/2.3).  Key details: one must supply the motion vectors from our pipeline into the `motionVectors` slot of the dispatch, and similarly set `jitterOffset = (0,0)` and `frameTimeDelta` appropriately.  No official API call expects us to supply depth or masks (they’re optional).  The user application is responsible for creating GPU textures with the right formats (typically R16G16 float for MVs, R32 for color, etc) and for linearizing sRGB if needed.  

In summary, **the FSR integration requires** AMD’s shipped FSR API DLLs (loader + upscaler), proper device compatibility, and using the documented `ffxDispatchDescUpscale` inputs (color, MV, jitter, etc) as above.  We cite AMD’s integration notes that “FSR Upscaling 4.1.1… must be included alongside amd_fidelityfx_loader.dll” and that the API “requires [the developer] to interact with the FSR SDK using the amd_fidelityfx_loader.dll”.  

# Prototype Implementation Plan  

A minimal experiment to test this idea could be built as follows:

- **Inputs:** A short high-resolution video (e.g. 4K source). Downsample it (e.g. with bicubic) to simulate a lower-res input (e.g. 1080p or 720p). Keep the original high-res as ground truth.  
- **Motion Vectors:** Implement two methods: (A) use FFmpeg to decode codec MVs for the low-res sequence; (B) run a pretrained optical-flow network (e.g. RAFT or FlowNet) on each frame pair. Export both results as full-frame MV maps.  
- **No-Jitter Baseline:** First, run FSR 4.1 with *no* MVs (all zeros) and `jitterOffset=(0,0)`, comparing the output to the ground truth.  
- **FSR with Synthetic MVs:** Then supply the codec MVs (converted to FSR format) to FSR and compare. Also try with the optical flow–derived MVs.  
- **Jitter Variants:** Since post-render jitter likely has little effect, one could optionally simulate a jitter sequence by, for example, cyclically shifting the low-res frames by ±0.5px (via resampling) and feeding those to FSR with corresponding `jitterOffset`. Compare that to no-jitter.  
- **Frame Generation Option:** (Advanced) Use FSR Frame Generation or a VFI network to create extra intermediate frames (effectively halving frameTimeDelta) and see if feeding those improves FSR’s result.

Implementation: Use AMD’s FSR SDK on Windows with either Vulkan or DX12. For motion, use PyTorch/OpenCV flows. Tools: FFmpeg for decoding, Python+GPU for flows, C++ for FSR (or possibly Python via PyNVAPI if available, but likely C++).  Measure processing time per stage (decode, flow, FSR) on representative hardware.

# Validation Methodology and Ablation Matrix  

To evaluate success, we compare the FSR output against the original high-res frames.  Set up controlled tests with e.g. a static scene, a panning scene, and a cut.  Use the following experimental conditions:

- **A. FSR only (no MVs, no jitter):** Tells us baseline (pure spatial SR/TAA).  
- **B. FSR + codec MV:** Use extracted MPEG/H.264 MVs only.  
- **C. FSR + optical flow MV:** Use dense flow (e.g. RAFT) vectors.  
- **D. FSR + “perfect” MV:** (If possible) Use known transforms (e.g. synthetic scene where motion is known) to simulate ideal MVs.  
- **E. Zero-jitter:** Run with `jitterOffset=(0,0)`. (This is default for all above.)  
- **F. FSR + shifted inputs:** (Optional) Run on inputs that have been shifted by ±0.5px and feed matching jitter offsets.  
- **G. FSR + true jitter samples:** (Offline) From the 4K ground truth, downsample at different subpixel offsets to simulate jittered renders, then feed those.  
- **H. FSR + temporal super-resolution:** (Optional) Try a video-SR method like Real-ESRGAN, for comparison.

**Metrics:** Compute framewise PSNR and SSIM between FSR output and ground truth, as well as perceptual LPIPS.  For temporal consistency, measure inter-frame differences or use a flicker metric (e.g. compute PSNR/SSIM between successive upscaled frames when compared under ideal motion alignment).  Qualitative checks: look for ghosting, blurring, flicker.  Use edge stability tests (e.g. sobel edges over time).  The goal is to see (1) FSR runs at all, (2) whether adding MVs reduces jitter/ghosts, (3) whether any scheme outperforms FSR alone or a spatial upscale (like AMD FSR2 or bicubic).  

# Real-Time Performance Assessment  

We estimate costs on a high-end GPU (e.g. AMD RDNA4 or NVIDIA RTX4xxx) for 1080p→4K and 720p→4K at 30 and 60 FPS:

- **FSR Upscaling:** According to AMD, FSR 4.1 Performance mode (2× scaling) runs **~1.3 ms at 4K** on an RX 9070 XT (a top-end RDNA4 card).  Even allowing ~3 ms on slightly older hardware, FSR itself is a small fraction of the frame time.  (Ultra mode 3× would be higher, but FSR still GPU-parallel.)  

- **Video Decoding:** Hardware decoders (NVDEC on NVIDIA, AMD VCN on Radeon) typically take on the order of 0.5–2 ms per 1080p frame at high settings.  Even for 4K60, modern decoders keep <5 ms.  So decoding a 1080p frame at 60 Hz is negligible (<1 ms).  

- **Motion Estimation:**  
  - *NVIDIA Optical Flow SDK:* If running on a Turing/Ampere/Ada GPU, NV’s dedicated unit can compute a forward or backward flow in ~2–3 ms for 1080p.  AMD GPUs have no equivalent, so rely on compute.  
  - *CNN Flow (RAFT/etc):* A pruned RAFT network on a GPU might do ~10–30 ms for 1080p (60+ ms on mid-tier hardware).  Very fast networks (TeCFlow, LiteFlowNet variants) claim tens of fps on high-end.  
  - *Classic flow (Farneback LK):* GPU-accelerated Farneback could be similar (~10 ms) with optimizations.  

- **Other Overheads:** Formatting, scaling MV textures, and dispatch calls might add a few ms. Jitter shift (if any) is negligible (just a simple resample).

Putting it together (1080p→4K at 60 Hz on top-end GPU):  
Decode: ~1 ms; Flow: ~3 ms (NV dedicated) or ~10–15 ms (compute); FSR: ~1–3 ms; overhead: ~1 ms.  Total ~6–20 ms.  This suggests **possible** at 60 fps on bleeding-edge hardware with fast flow (NVIDIA’s unit or very optimized compute).  At 30 fps, of course easier.  

For 720p→4K (3× scale) at 60 Hz: decode ~1 ms; flow on 720p is faster (smaller image); FSR Ultra mode might be ~2 ms; total probably ~5–15 ms.  So also plausible on top-end.  

However, on mid-range hardware the flow step likely dominates.  The bottom line: **real-time is challenging but feasible on high-end GPUs**.  FSR itself is cheap; the motion step is the biggest unknown cost.

# Known Failure Modes and Mitigation  

- **Occlusions/Disocclusions:** New objects or uncovered regions yield no valid motion.  We can mitigate by detecting large MV divergence and using `reset` or zeroing vectors.  FSR will then fall back on spatial-only for those pixels.  
- **Cut or Rapid Change:** When the scene abruptly changes (cut, fade), FSR’s history is stale.  Set `reset=true` for that frame.  We must implement a robust scene-cut detector to catch these.  
- **Motion Blur:** Fast-moving objects blur between frames.  Neither codec MV nor flow handles blur well.  Best we can do is drop confidence and blur blending.  FSR’s temporal stability claim may mitigate minor blur, but heavy blur is trouble.  
- **Repeated Patterns/Aliasing:** Flow may confuse repeated textures.  We could fall back to zero MV in low-texture areas.  
- **Compression Artifacts:** Intra-frame noise or compression blocks can generate spurious vectors.  Pre-filtering or using robust confidence measures on MVs helps.  
- **No Jitter:** Without actual jitter, FSR’s ability to recover subpixel detail is limited.  We treat jitter as zero (see above).  Expect slightly less sharpness than in a jittered-game pipeline.  

Each of these can be partially handled by conservative vector filtering and by FSR’s internal blending.  The AMD docs emphasize that FSR 4 is *already improved in ghosting/temporal stability* over FSR3, suggesting it is somewhat robust, but our inputs are weaker than a game’s, so artifacts will still occur.

# Comparison with Alternative Approaches  

For prerendered video, many alternative upscaling methods exist:  

- **Spatial-only SR:** Non-temporal super-resolution networks (e.g. ESRGAN/EDSR/FSRCNN) operate on each frame independently.  They can produce high-quality detail but tend to create temporal incoherence (flicker) unless specifically stabilized.  They are not real-time on high resolution (usually slower). AMD’s **Lemonade** framework with Real-ESRGAN (and AMD NPU acceleration) does 4× SR locally, but it’s offline (used via API, not real-time).  

- **Dedicated Video SR:** Modern video-SR networks (VESPCN, FRVSR, EDVR, BasicVSR, etc.) explicitly use temporal information for consistency.  They often outperform naive frame-by-frame SR in quality.  However, real-time versions are rare; most research models are too large for live processing.  Some simpler ones (like VESPCN) claim real-time on high-end GPUs.  

- **Optical-Flow Super-Resolution:** There are approaches (e.g. TOFlow, FlyNet) that warp neighboring frames before SR.  These are similar in spirit to what we’d do, but built end-to-end as neural nets.  In principle, one could use a pre-trained video-SR model instead of FSR.  

- **NVIDIA/AMD Upscaling:** DLSS (NVIDIA) and XeSS (Intel/AMD) are primarily designed for game scenes. AMD also has **Super Resolution (FSR 2)** built into Radeon drivers for any app, but that’s only spatial.  None of these ingest ordinary video either, except as part of games.  Microsoft has announced **AI Video Super Resolution** for Copilot+ PCs, which likely uses a dedicated ML model on CPU/GPU to upsample video in real time. That is a direct competitor: it’s designed for network streams and video apps (e.g. Teams webcam).  On AMD hardware, the AMD blog notes that Microsoft’s VSR is available and accelerated on Ryzen AI.  

In summary, adapting FSR 4 to video is a *hybrid hack*.  True video-focused methods (ML video SR or the OS’s built-in VSR) may yield better visual consistency since they’re trained on frame sequences.  FSR’s advantage is that it’s already optimized for low-latency on high-end GPUs (thanks to AMD’s ML accelerator), but it wasn’t designed for arbitrary footage.  In terms of quality, a well-tuned video-SR network (especially one trained on real footage) will likely give sharper, more stable results.  But FSR might outperform naive upscaling (e.g. lanczos) or even AMD’s FSR2 in a pinch.  

# Open Questions and Evidence Gaps  

- **MV Quality vs. Benefit:** It remains to be seen quantitatively how much improving the MV field actually helps FSR’s output quality.  Does dense optical flow significantly reduce ghosting compared to zero MVs?  We need experiments (above) to answer this.  
- **FSR’s internal handling:** AMD’s documentation doesn’t detail how FSR internally handles invalid MVs or occlusions.  We assume zero fill/reset.  If we find their implementation or tests (e.g. via GPUOpen samples), that could refine our approach.  
- **Jitter necessity:** Without true jittered input, is the concept of “jitter offset” in dispatch moot?  Likely yes.  We should confirm whether FSR automatically treats jitter offset=0 as “no subpixel sampling”.  
- **Other inputs:** Depth and masks are optional; likely not needed.  The role of an “exposure” input is unclear for video.  Possibly irrelevant (we can use linear frames and no auto-exposure).  
- **Hardware support:** FSR 4 requires RDNA4/7000 series.  For a broader solution, one might fall back to FSR3.1 on older GPUs.  How does FSR3 differ in inputs? Likely similar (it also used MVs and jitter) but one would need to check FSR3 docs separately if targeting old GPUs.  
- **Legal/licensing:** Using the official AMD FSR DLLs should be allowed on PC, but this whole pipeline is essentially a mod – we should check that using FSR on video (outside a game) doesn’t violate any license terms.  
- **Latency:** Our analysis focused on throughput.  Real-time upscaling of a **live** stream (e.g. Twitch) adds network and decode latency.  If we aim for <33ms end-to-end, MV and FSR must be very fast. A practical implementation might need to drop to 30 fps to be safe.  

# Final Recommendation  

In our assessment, adapting FSR 4.1 as a video upscaler is **possible but highly experimental**.  The API can be driven with fabricated inputs, and a prototype system can be built that *runs* (FSR has minimal FPS cost).  However, key features – true motion vectors and jittered sampling – are absent or weak in pre-recorded video.  As a result, the “quality” gains are uncertain: FSR may reconstruct edges slightly better than plain upscaling and might reduce temporal flicker if good MVs are provided, but it will likely also produce new artifacts where flows are wrong.  Compared to mature video-specific super-resolution solutions (including upcoming OS-level VSR), the effort to retrofit a game-oriented upscaler does not appear to offer a clear win. 

**Classification:** *“Possible but highly experimental”.*  We can **make FSR 4 run on video**, but preserving/increasing visual quality beyond advanced spatial-only SR is doubtful without further innovation.  The biggest obstacle is the lack of genuine new information (no real subpixel jitter, no ground-truth motion).  Unless a use-case absolutely demands using AMD’s FSR library, a dedicated video SR approach will likely be more justified.

**Sources:** Official AMD GPUOpen FSR 4.1 docs; AMD developer blogs; FFmpeg/codec resources. These confirm the above requirements and considerations.