# Native base-coordinate jitter probe

This probe compares the native INT8 path's normal stable base-coordinate
resolve with the opt-in `TFORGE_FSR4_CURRENT_BASE_JITTERED=1` variant. It tests
whether restoring the jittered source phase improves reconstruction without
changing the learned path.

The candidate is rejected. It improves the measured temporal error on Sintel
cave, but substantially worsens Tears of Steel daylight and lowers its SSIM.
The default unjittered base coordinate remains unchanged.
