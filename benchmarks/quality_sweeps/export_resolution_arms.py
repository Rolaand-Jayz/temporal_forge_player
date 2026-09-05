#!/usr/bin/env python3
"""Export every captured FSR/CAS arm for one resolution pair.

The three roots must be independently captured: pre-CAS, post-CAS, and no-CAS.
This intentionally refuses to manufacture missing arms by copying a different
pipeline's image under another method ID.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

SCENES = ("tos_daylight", "sintel_rooftop", "sintel_cave")
SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pre-root", type=Path, required=True)
    parser.add_argument("--post-root", type=Path, required=True)
    parser.add_argument("--no-cas-root", type=Path, required=True)
    parser.add_argument("--input", type=int, required=True, dest="input_height")
    parser.add_argument("--output", type=int, required=True, dest="output_height")
    parser.add_argument("--frame", default="0048")
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--harness-root", type=Path, default=Path("review_harness"))
    args = parser.parse_args()
    roots = {"pre": args.pre_root, "post": args.post_root, "no_cas": args.no_cas_root}
    root = args.repo.resolve()
    exporter = root / "tools/export_review_image.py"
    count = 0
    for placement, artifact_root in roots.items():
        for scale in SCALES:
            scale_dir = artifact_root / f"scale_{scale:.2f}".replace(".", "_")
            suffix = "cas20_pre" if placement == "pre" else ("cas20_post" if placement == "post" else "no_cas")
            method = f"fsr_{int(scale * 100):03d}x_downsample_{suffix}"
            for scene in SCENES:
                source = scale_dir / scene / "candidate_final.png"
                if not source.is_file():
                    raise SystemExit(f"missing independently captured arm: {source}")
                subprocess.run([
                    sys.executable, str(exporter), str(source), "--scene", scene,
                    "--frame", args.frame, "--input", str(args.input_height),
                    "--method", method, "--output", str(args.output_height),
                    "--root", str(args.harness_root.resolve()),
                ], cwd=root, check=True)
                count += 1
    print(f"exported {count} independent FSR/CAS arms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
