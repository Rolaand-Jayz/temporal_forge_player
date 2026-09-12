# Clean learned-strength 0.10 candidate

This opt-in candidate was captured after isolating the temporal runner from
the checkout-level interactive Quality Lab file.

- native INT8 graph
- software decode
- 426x240 input to 1920x1080 output
- 8 scored frames
- `TFORGE_FSR4_LEARNED_STRENGTH=0.10`
- four real scenes only

It improves spatial SSIM over the clean 0.05 default on all four scenes, but
temporal error is mixed. It remains an opt-in candidate and is not promoted as
the playback default.
