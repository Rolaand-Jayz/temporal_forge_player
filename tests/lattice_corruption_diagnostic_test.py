#!/usr/bin/env python3
"""Static regression guard for the bounded lattice-corruption diagnostic path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
uploader = (ROOT / "src/render/GpuImageUploader.cpp").read_text()
engine = (ROOT / "src/core/PlaybackEngine.cpp").read_text()
shader = (ROOT / "shaders/fsr4/bicubic_prefilter.comp").read_text()
runner = (ROOT / "benchmarks/quality_sweeps/lattice_corruption_diagnostic/run_lattice_diagnostic.sh").read_text()

assert 'TFORGE_FSR4_REFERENCE_RESIZE' in uploader
assert 'TFORGE_FSR4_DUMP_STAGE_DIR' in engine
assert 'dispatchBicubicPrefilter(frame)' in uploader
assert 'filterScale = min(scale, vec2(1.0))' in shader
assert 'for (int x = -4; x <= 5; ++x)' in shader
assert 'source_model_resampler=' in uploader
assert 'run_case bad_gpu 1280x720 1920x1080' in runner
assert 'run_case healthy_gpu 640x360 1280x720' in runner
assert 'run_case bad_reference 1280x720 1920x1080 1' in runner
assert '540' not in runner
print('lattice corruption diagnostic contract: PASS')
