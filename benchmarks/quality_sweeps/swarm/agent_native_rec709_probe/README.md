# Native Rec.709 input-transfer probe

This probe tests the opt-in explicit Rec.709 inverse transfer before the
native INT8 FSR path. It uses the same four real scenes, 426x240 inputs,
1920x1080 outputs, and twelve scored frames per pair.

The candidate is rejected. It lowers SSIM on all four scenes. It improves
temporal error on rooftop and cave, but the spatial losses are too large and
daylight remains worse. The default transfer path remains unchanged.
