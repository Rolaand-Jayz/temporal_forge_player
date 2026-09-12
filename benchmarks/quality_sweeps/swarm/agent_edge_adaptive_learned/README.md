# Edge-adaptive learned blend

This candidate is an opt-in postpass composition probe. It keeps the learned
strength in smooth source regions, but reduces it near strong source edges so
the spatial base preserves fine detail.

The attenuation is controlled by `TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED_STRENGTH`.
The tested middle value was `0.50`.

## Configuration

- `TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED=1`
- `TFORGE_FSR4_EDGE_ADAPTIVE_LEARNED_STRENGTH=0.50`
- software decode, color history enabled
- learned strength `0.35`
- 8 warmup frames, 12 scored frames
- real 426x240 inputs, 1920x1080 output

The fixed comparison used the same settings without the edge-adaptive switch.

## Result

Rejected for promotion. The candidate improved SSIM on all four real scenes,
but worsened temporal absolute error on all four versus its matched fixed
control. It remains available only for controlled reproduction while the
temporal composition is investigated.

