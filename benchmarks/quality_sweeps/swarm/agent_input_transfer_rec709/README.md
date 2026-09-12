# Input-transfer interpretation candidate

This was a matched real-scene A/B of `TFORGE_FSR4_INPUT_TRANSFER=rec709`
against the existing software-decode, color-history, learned-strength `0.15`
path. It changes only the input transfer interpretation before the FSR model.

The candidate was rejected. On the 426x240 Tears of Steel daylight clip it
lowered SSIM and did not improve temporal error, so the current transfer path
remains unchanged.
