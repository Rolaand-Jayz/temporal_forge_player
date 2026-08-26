# Bilinear plus fixed-history-weight A/B

This is a matched real-corpus test of whether the reference-style fixed `0.1`
current-frame history weight can recover the temporal penalty observed when the
base reconstruction filter is changed from Catmull-Rom to bilinear.

Both arms use the native INT8 graph, software decode, color history, the same
`base_only` composition, and 12 warm-up plus 12 scored frames. Only the base
filter differs. The fixed-history flag is shared by both arms.

The candidate raises spatial SSIM on all four scenes, but temporal absolute
error also rises on all four: daylight `1.487515 -> 1.748300`, debris
`0.012909 -> 0.325521`, rooftop `0.544263 -> 1.395563`, and cave
`0.624852 -> 0.709602`. It is rejected and not promoted.

The compact measurements are in `measured_results.json`; the paired CSVs are
under `captures/`.
