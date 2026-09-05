#!/usr/bin/env python3
"""Matched-control periodic-lattice check: candidate vs baseline frames."""
import sys, glob, os
sys.path.insert(0, 'benchmarks/quality_sweeps/lattice_corruption_diagnostic')
from pathlib import Path
import periodic_lattice_detector as det
root = Path(sys.argv[1])  # e.g. .competition/e9_final_matrix
pairs = {}
for cand in sorted(root.glob('*_candidate/artifacts/fsr_frames/*.ppm')):
    rel = cand.relative_to(root).parents[2].name  # scene_tier_candidate
    base_dir = root / rel.replace('_candidate', '_baseline') / 'artifacts' / 'fsr_frames'
    base_frames = sorted(base_dir.glob('*.ppm'))
    idx = int(cand.stem.split('_')[-1])
    if idx < len(base_frames):
        pairs[cand] = base_frames[idx]
results = []
for cand, base in sorted(pairs.items()):
    if int(cand.stem.split('_')[-1]) % 4 != 0:  # sample every 4th frame
        continue
    r = det.analyze(cand, [base])
    r['pair'] = f'{cand.relative_to(root)}'
    if not r['matched_control_residuals']:
        r['classification'] = 'control_missing'
    results.append(r)
worst = max(results, key=lambda r: max((row['residual_autocorrelation_energy'] for row in r['matched_control_residuals']), default=0.0))
import json
summary = {
    'pairs_checked': len(results),
    'classified': {},
    'worst_pair': worst['pair'],
    'worst_residual_energy': max((row['residual_autocorrelation_energy'] for row in worst['matched_control_residuals']), default=0),
    'threshold': det.RESIDUAL_LATTICE_THRESHOLD,
}
for r in results:
    summary['classified'][r['classification']] = summary['classified'].get(r['classification'], 0) + 1
out = root / 'lattice_check.json'
out.write_text(json.dumps({'summary': summary, 'results': results}, indent=1))
print(json.dumps(summary, indent=1))
