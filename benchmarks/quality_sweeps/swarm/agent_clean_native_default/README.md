# Clean native default baseline

This is the corrected real-corpus baseline after the temporal runner stopped
loading the checkout's interactive `config/quality_lab.json` implicitly.

- native INT8 graph
- software decode
- 426x240 input to 1920x1080 output
- 8 scored frames
- Current composition defaults
- no synthetic scenes

The four CSVs are copied directly from the completed captures. The runner
still honors an explicit `TFORGE_QUALITY_LAB_CONFIG` when a deliberate Quality
Lab experiment is requested.
