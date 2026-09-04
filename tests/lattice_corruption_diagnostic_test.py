#!/usr/bin/env python3
"""Static regression guard for the bounded lattice-corruption diagnostic path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
uploader = (ROOT / "src/render/GpuImageUploader.cpp").read_text()
engine = (ROOT / "src/core/PlaybackEngine.cpp").read_text()
shader = (ROOT / "shaders/fsr4/bicubic_prefilter.comp").read_text()
runner = (ROOT / "benchmarks/quality_sweeps/lattice_corruption_diagnostic/run_lattice_diagnostic.sh").read_text()
precamp = (ROOT / "benchmarks/quality_sweeps/run_precampaign_qualification.py").read_text()
supersampling = (ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py").read_text()
engine_capture = (ROOT / "benchmarks/quality_sweeps/capture_engine.py").read_text()
campaign = (ROOT / "benchmarks/quality_sweeps/run_harness_campaign.py").read_text()

assert 'TFORGE_FSR4_REFERENCE_RESIZE' in uploader
assert 'TFORGE_FSR4_DUMP_STAGE_DIR' in engine
assert 'dispatchBicubicPrefilter(frame)' in uploader
assert 'if (scale.x < 1.0 || scale.y < 1.0)' in shader
assert 'vec3 p00 = loadSource(base, sourceSize);' in shader
assert 'for (int x = -1; x <= 2; ++x)' in shader
prepass = (ROOT / "shaders/fsr4/prepass_pq_eotf.comp").read_text()
# Geometry mismatch remains an explicit scale-aware path, but no longer forces
# a per-frame history reset; publication of the current resolve keeps the
# temporal state stable across the mismatch.
assert 'sourceToModelScale' in prepass
assert 'const bool fullReset = (slot0.z & 2u) != 0u;' in prepass
assert 'imageStore(u_historyOut, coord, vec4(upscaledColor, 1.0));' in (ROOT / "shaders/fsr4/postpass_composite.comp").read_text()
assert 'source_model_resampler=' in uploader
assert 'if (modelW_ != srcW_ || modelH_ != srcH_)' in uploader
assert 'CASES = ((360, 720), (360, 1080), (720, 1080))' in precamp
assert 'TFORGE_FSR4_REFERENCE_RESIZE' in precamp or 'TFORGE_FSR4_REFERENCE_RESIZE' in runner
assert 'from benchmarks.quality_sweeps.capture_engine import run_renderer' in precamp
assert 'score = checker_score(image)' in precamp
assert 'from benchmarks.quality_sweeps.capture_engine import run_renderer' in campaign
assert 'def run_renderer(' in engine_capture
assert 'run_case bad_gpu 1280x720 1920x1080' in runner
assert 'run_case healthy_gpu 640x360 1280x720' in runner
assert 'run_case bad_reference 1280x720 1920x1080 1' in runner
assert '540' not in runner
assert '--history' in supersampling and '--recurrent' in supersampling
assert 'expected_history=args.history == "on"' in supersampling
assert 'expected_recurrent=args.recurrent == "on"' in supersampling
print('lattice corruption diagnostic contract: PASS')
