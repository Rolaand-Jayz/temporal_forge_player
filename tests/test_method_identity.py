"""Contract tests for producer arm id -> canonical campaign method mapping."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "benchmarks" / "quality_sweeps" / "method_identity.py"

_spec = importlib.util.spec_from_file_location("method_identity_under_test", MODULE)
method_identity = importlib.util.module_from_spec(_spec)
sys.modules.setdefault("method_identity_under_test", method_identity)
_spec.loader.exec_module(method_identity)

canonical_method_for_arm = method_identity.canonical_method_for_arm
UnknownArmId = method_identity.UnknownArmId


class CanonicalMethodMappingTests(unittest.TestCase):
    def test_saved_preset_full_vocabulary(self):
        cases = {
            "saved_2.00x_cas20_pre": "fsr_200x_downsample_resolve_cas20",
            "saved_2.00x_cas20_post": "fsr_200x_downsample_external_post_cas20",
            "saved_2.00x_no_cas": "fsr_200x_downsample_no_cas",
            "saved_2.25x_cas20_pre": "fsr_225x_downsample_resolve_cas20",
            "saved_2.50x_cas20_post": "fsr_250x_downsample_external_post_cas20",
            "saved_2.75x_no_cas": "fsr_275x_downsample_no_cas",
            "saved_3.00x_cas20_pre": "fsr_300x_downsample_resolve_cas20",
        }
        for arm_id, expected in cases.items():
            self.assertEqual(canonical_method_for_arm(arm_id), expected, arm_id)

    def test_nativeaa_preset_ignores_scale_tier(self):
        self.assertEqual(
            canonical_method_for_arm("nativeaa_2.00x_cas20_pre"),
            "fsr_nativeaa_downsample_resolve_cas20",
        )
        self.assertEqual(
            canonical_method_for_arm("nativeaa_2.00x_no_cas"),
            "fsr_nativeaa_downsample_no_cas",
        )

    def test_unknown_scale_fails_instead_of_guessing(self):
        with self.assertRaises(UnknownArmId):
            canonical_method_for_arm("saved_1.33x_cas20_pre")

    def test_unknown_cas_strength_fails_instead_of_guessing(self):
        with self.assertRaises(UnknownArmId):
            canonical_method_for_arm("saved_2.00x_cas30_pre")

    def test_malformed_arm_ids_fail(self):
        for bad in ("", "saved", "saved_2.00x", "saved_2x_cas20_pre",
                    "saved_2.00x_cas20_sideways", "Saved_2.00x_no_cas",
                    "saved_2.00x_no_cas_extra"):
            with self.assertRaises(UnknownArmId, msg=bad):
                canonical_method_for_arm(bad)

    def test_every_mapped_method_matches_review_harness_vocabulary(self):
        canonical_names = {
            "fsr_200x_downsample_resolve_cas20", "fsr_200x_downsample_external_post_cas20",
            "fsr_200x_downsample_no_cas", "fsr_225x_downsample_resolve_cas20",
            "fsr_225x_downsample_external_post_cas20", "fsr_225x_downsample_no_cas",
            "fsr_250x_downsample_resolve_cas20", "fsr_250x_downsample_external_post_cas20",
            "fsr_250x_downsample_no_cas", "fsr_275x_downsample_resolve_cas20",
            "fsr_275x_downsample_external_post_cas20", "fsr_275x_downsample_no_cas",
            "fsr_300x_downsample_resolve_cas20", "fsr_300x_downsample_external_post_cas20",
            "fsr_300x_downsample_no_cas", "fsr_nativeaa_downsample_resolve_cas20",
            "fsr_nativeaa_downsample_external_post_cas20", "fsr_nativeaa_downsample_no_cas",
        }
        arms = [
            f"saved_{scale}_{cas}"
            for scale in ("2.00x", "2.25x", "2.50x", "2.75x", "3.00x")
            for cas in ("cas20_pre", "cas20_post", "no_cas")
        ] + [
            f"nativeaa_2.00x_{cas}"
            for cas in ("cas20_pre", "cas20_post", "no_cas")
        ]
        mapped = {canonical_method_for_arm(arm) for arm in arms}
        self.assertEqual(mapped, canonical_names)


if __name__ == "__main__":
    unittest.main()
