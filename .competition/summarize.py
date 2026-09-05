#!/usr/bin/env python3
"""Join all competition capture arms into one comparison table.

Reads every quality.csv under .competition/*/**/quality.csv and prints
SSIM (vs Lanczos control), min SSIM, temporal delta error (vs reference
delta) and the Lanczos delta error, keyed by scene/tier/arm.
"""
import csv
import glob
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
rows = []
for path in sorted(glob.glob(os.path.join(ROOT, '*', '*', 'quality.csv'))):
    rel = os.path.relpath(os.path.dirname(path), ROOT)
    exp, name = rel.split(os.sep, 1)
    try:
        with open(path) as fh:
            record = list(csv.DictReader(fh))
        if not record:
            continue
        r = record[-1]
        rows.append((exp, name, float(r['fsr_ssim_mean']),
                     float(r['lanczos_ssim_mean']), float(r['fsr_ssim_min']),
                     float(r['fsr_temporal_delta_mean']),
                     float(r['reference_temporal_delta_mean']),
                     float(r['fsr_temporal_delta_abs_error']),
                     float(r['lanczos_temporal_delta_abs_error'])))
    except (OSError, KeyError, ValueError) as error:
        print(f'{rel}: unreadable ({error})', file=sys.stderr)

print(f'{"experiment":14} {"arm":44} {"ssim":>9} {"lanczos":>9} {"d_ssim":>9} '
      f'{"min":>9} {"derr":>8} {"lz_derr":>8}')
for (exp, name, ssim, lz, ssim_min, _delta, _ref, derr, lz_derr) in rows:
    print(f'{exp:14} {name:44} {ssim:9.6f} {lz:9.6f} {ssim - lz:+9.6f} '
          f'{ssim_min:9.6f} {derr:8.4f} {lz_derr:8.4f}')
