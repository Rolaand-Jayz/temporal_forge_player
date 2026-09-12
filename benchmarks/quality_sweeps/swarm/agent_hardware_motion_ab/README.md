# Hardware decode versus codec-motion A/B

This diagnostic isolates the current VAAPI quality gap. The hardware path on
this AMD setup produces no `AV_FRAME_DATA_MOTION_VECTORS`; software decode of
the same encoded clip does. Without those vectors, the FSR motion-validity
texture is uncovered and display-color history cannot be reused safely.

The capture used the same 426x240 high-quality Tears of Steel daylight clip,
software/hardware decode as the only variable, color history enabled, eight
warmup frames, and twelve scored frames at 1920x1080.

This is evidence for a future quality-mode decision, not a default change.

The matched timing check does not support a CPU-bottleneck explanation for the
FSR path. At 1920x1080, hardware decode measured about 0.006 ms decode and
0.125 ms upload per frame; software decode measured about 0.172 ms decode and
0.169 ms upload. GPU work was about 5.7 ms in both cases, while command
recording was about 0.03 ms. A six-second hardware run used about 7% total CPU.
