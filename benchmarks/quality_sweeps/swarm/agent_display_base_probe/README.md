# Display-base strength probe

This probe tests the existing display-base control as a spatial/temporal
composition tradeoff. It does not change the default path.

On the daylight scene, increasing display-base strength consistently recovered
some spatial SSIM but increased temporal error. No fixed strength is promoted;
the next useful version would need motion-aware gating rather than one global
blend value.
