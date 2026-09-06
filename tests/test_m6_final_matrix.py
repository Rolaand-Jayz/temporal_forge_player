#!/usr/bin/env python3
"""Contract tests for the authoritative M6 matrix assembler."""

import unittest

from tools.assemble_m6_final_matrix import _numeric_metrics


class M6FinalMatrixContractTests(unittest.TestCase):
    """Keep numeric normalization from corrupting provenance metadata."""

    def test_numeric_metrics_preserves_string_asset_paths(self) -> None:
        row = {"metrics": {"ssim": "0.7", "full_output_path": "/tmp/frame.png"}}

        normalized = _numeric_metrics(row)

        self.assertEqual(normalized["metrics"]["ssim"], 0.7)
        self.assertEqual(normalized["metrics"]["full_output_path"], "/tmp/frame.png")


if __name__ == "__main__":
    unittest.main()
