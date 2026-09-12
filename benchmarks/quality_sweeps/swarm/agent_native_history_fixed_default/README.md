# Native INT8 fixed-history check

This is a matched A/B of the clean current native INT8 playback path against
the same path with display-color history enabled and the experimental fixed
history weight enabled.

The test used four real scenes at 426x240 input and 1920x1080 output, with 12
warmup frames and 12 scored frames. Both arms selected the native INT8 graph
and used software decode. The paired CSVs are in `captures/`.

All four CSV pairs are byte-identical. The candidate is therefore rejected as
a no-op on native playback. A prior Quality Lab result must not be promoted
from this evidence: that result exercised the generic composition path, while
the native graph has a separate descriptor set and 14-pass buffer graph.

No reconstruction shader, model, or image-quality behavior was changed for
this measurement.
