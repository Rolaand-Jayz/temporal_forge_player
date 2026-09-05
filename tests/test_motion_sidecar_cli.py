"""Command-level tests for assembling player motion frame records."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MotionSidecarCliTests(unittest.TestCase):
    """Keep the capture bridge usable without importing benchmark internals."""

    def test_cli_assembles_complete_sidecar_and_refuses_missing_records(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            for index, available in enumerate((False, True)):
                (records / f"codec_motion_{index:04d}.json").write_text(
                    json.dumps({
                        "frameIndex": index,
                        "ptsUs": index * 33333,
                        "reset": index == 0,
                        "motionAvailable": available,
                        "vectors": [] if not available else [{
                            "dstX": 0,
                            "dstY": 0,
                            "mvX": 0.0,
                            "mvY": 0.0,
                            "w": 2,
                            "h": 1,
                            "source": -1,
                        }],
                        "sourceWidth": 2,
                        "sourceHeight": 1,
                    }),
                    encoding="utf-8",
                )
            output = root / "motion.json"
            result = subprocess.run(
                [
                    sys.executable,
                    "tools/assemble_motion_sidecar.py",
                    "--records-dir",
                    str(records),
                    "--expected-frames",
                    "2",
                    "--target-width",
                    "4",
                    "--target-height",
                    "2",
                    "--output",
                    str(output),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            sidecar = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(sidecar["schema"], "temporal_forge.codec_motion.v1")
            self.assertEqual(len(sidecar["frames"]), 2)

            output.unlink()
            (records / "codec_motion_0001.json").unlink()
            missing = subprocess.run(
                [
                    sys.executable,
                    "tools/assemble_motion_sidecar.py",
                    "--records-dir",
                    str(records),
                    "--expected-frames",
                    "2",
                    "--target-width",
                    "4",
                    "--target-height",
                    "2",
                    "--output",
                    str(output),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("frame", missing.stderr.lower())


if __name__ == "__main__":
    unittest.main()
