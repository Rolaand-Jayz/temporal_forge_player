#!/usr/bin/env python3
"""Static regression guard for the bounded lattice-corruption diagnostic path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
uploader = (ROOT / "src/render/GpuImageUploader.cpp").read_text()
engine = (ROOT / "src/core/PlaybackEngine.cpp").read_text()
shader = (ROOT / "shaders/fsr4/bicubic_prefilter.comp").read_text()
runner = (ROOT / "benchmarks/quality_sweeps/lattice_corruption_diagnostic/run_lattice_diagnostic.sh").read_text()
precamp = (ROOT / "benchmarks/quality_sweeps/run_precampaign_qualification.py").read_text()
engine_capture = (ROOT / "benchmarks/quality_sweeps/capture_engine.py").read_text()
campaign = (ROOT / "benchmarks/quality_sweeps/run_harness_campaign.py").read_text()

assert 'TFORGE_FSR4_REFERENCE_RESIZE' in uploader
assert 'TFORGE_FSR4_DUMP_STAGE_DIR' in engine
assert 'dispatchBicubicPrefilter(frame)' in uploader
assert 'filterScale = min(scale, vec2(1.0))' in shader
assert 'for (int x = -4; x <= 5; ++x)' in shader
assert 'sourceModelGeometryMismatch' in (ROOT / "shaders/fsr4/prepass_pq_eotf.comp").read_text()
assert 'source_model_resampler=' in uploader
assert 'if (modelW_ != srcW_ || modelH_ != srcH_)' in uploader
assert 'CASES = ((360, 720), (360, 1080), (720, 1080))' in precamp
assert 'TFORGE_FSR4_REFERENCE_RESIZE' in precamp or 'TFORGE_FSR4_REFERENCE_RESIZE' in runner
assert 'from benchmarks.quality_sweeps.capture_engine import run_renderer' in precamp
assert 'from benchmarks.quality_sweeps.capture_engine import run_renderer' in campaign
assert 'def run_renderer(' in engine_capture
assert 'run_case bad_gpu 1280x720 1920x1080' in runner
assert 'run_case healthy_gpu 640x360 1280x720' in runner
assert 'run_case bad_reference 1280x720 1920x1080 1' in runner
assert '540' not in runner
print('lattice corruption diagnostic contract: PASS')
