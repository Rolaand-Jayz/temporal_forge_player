"""Contract tests for the distributable, data-driven still reviewer."""

from __future__ import annotations

import base64
import json
import os
import re
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


# A tiny valid PNG keeps this builder contract test independent of benchmark
# image size. The test exercises discovery and manifest wiring, not image
# quality; the real corpus remains the source for human review.
_ONE_PIXEL_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk"
    "+A8AAQUBAScY42YAAAAASUVORK5CYII="
)


def _png_with_dimensions(width: int, height: int) -> bytes:
    """Keep the fixture tiny while giving discovery realistic PNG dimensions."""

    data = bytearray(_ONE_PIXEL_PNG)
    data[16:20] = struct.pack(">I", width)
    data[20:24] = struct.pack(">I", height)
    return bytes(data)


class ReviewHarnessContractTests(unittest.TestCase):
    """Keep the standalone reviewer truthful and independently mirrored."""

    def test_builder_emits_structured_real_asset_manifest_and_split_controls(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            results = root / "results"
            review_root = results / "review_fixture_frame1"
            review_root.mkdir(parents=True)
            names = (
                "tos_daylight_426x240_high_crf12_review.png",
                "tos_daylight_426x240_reference_1920x1080_f48.png",
                "tos_daylight_reference_1920x1080_f55.png",
                "tos_daylight_426x240_to1920x1080_temporal_forge_review.png",
                "tos_daylight_426x240_f48_m6-current-20260823T193252Z-01-current.png",
                "tos_daylight_426x240_f48_m6-current-20260823T193252Z-01-current_bicubic.png",
                "tos_daylight_426x240_f48_m6-current-20260823T193252Z-01-current_gpu_raw.png",
                "tos_daylight_426x240_to1920x1080_stagec-detail_residual_075_adaptive_sharpen_020_review.png",
                "synthetic_motion_426x240_to1920x1080_temporal_forge_review.png",
            )
            for name in names:
                dimensions = (426, 240) if "high_crf12" in name else (1920, 1080)
                (review_root / name).write_bytes(_png_with_dimensions(*dimensions))

            input_html = root / "legacy.html"
            input_html.write_text(
                "const embeddedAssets = Object.freeze({});\n",
                encoding="utf-8",
            )
            output_html = root / "review.html"
            environment = os.environ.copy()
            environment["TFORGE_REVIEW_RESULTS_ROOT"] = str(results)
            subprocess.run(
                [
                    "node",
                    "tools/build_review_harness.mjs",
                    str(input_html),
                    str(output_html),
                ],
                check=True,
                cwd=Path(__file__).resolve().parents[1],
                env=environment,
                capture_output=True,
                text=True,
            )

            html = output_html.read_text(encoding="utf-8")
            manifest_match = re.search(
                r"const assetManifest = Object\.freeze\((\[[\s\S]*?\])\);",
                html,
            )
            self.assertIsNotNone(manifest_match)
            manifest = json.loads(manifest_match.group(1))
            self.assertTrue(manifest)
            self.assertTrue(all("scene" in asset for asset in manifest))
            self.assertTrue(all("family" in asset for asset in manifest))
            self.assertTrue(all("inputResolution" in asset for asset in manifest))
            self.assertTrue(all("imageResolution" in asset for asset in manifest))
            self.assertTrue(all("outputResolution" in asset for asset in manifest))
            self.assertTrue(all("technique" in asset for asset in manifest))
            self.assertTrue(all("assetPath" in asset for asset in manifest))
            self.assertFalse(any("synthetic" in asset["scene"] for asset in manifest))
            self.assertFalse(any(asset["assetName"].endswith("_gpu_raw.png") for asset in manifest))
            techniques = {asset["technique"] for asset in manifest}
            self.assertIn("native-reference", techniques)
            contextual_reference = next(
                asset for asset in manifest if "426x240_reference" in asset["assetName"]
            )
            self.assertEqual(contextual_reference["inputResolution"], "426x240")
            self.assertEqual(contextual_reference["outputResolution"], "1920x1080")
            current = next(asset for asset in manifest if asset["technique"] == "temporal-forge")
            self.assertIsNone(current["experimentId"])
            campaign_current = next(
                asset for asset in manifest if asset["assetName"].endswith("current.png")
            )
            self.assertEqual(campaign_current["technique"], "temporal-forge")
            campaign_bicubic = next(
                asset for asset in manifest if asset["assetName"].endswith("current_bicubic.png")
            )
            self.assertEqual(campaign_bicubic["technique"], "bicubic-control")
            native = next(asset for asset in manifest if asset["technique"] == "native-input")
            self.assertIsNone(native["featureStack"])
            self.assertIsNone(native["featureStackKey"])
            experimental = next(asset for asset in manifest if asset["technique"] == "experimental")
            self.assertAlmostEqual(experimental["residualStrength"], 0.75)
            self.assertEqual(experimental["sharpeningMode"], "Adaptive")
            self.assertAlmostEqual(experimental["sharpeningStrength"], 0.20)
            self.assertTrue(any(asset["technique"] == "experimental" for asset in manifest))
            self.assertTrue(any(asset["featureStackKey"] for asset in manifest if asset["technique"] == "experimental"))
            self.assertIn('const state={left:', html)
            self.assertIn("compare-stage", html)
            self.assertIn("Upscale technique", html)
            self.assertIn("const applicable=modifierFields.filter", html)
            self.assertIn("a[f]===''", html)
            self.assertIn('data-view="pixel"', html)
            self.assertIn("writeHash()", html)
            self.assertIn("featureStackKey", html)
            self.assertIn("const featureOrder=", html)
            self.assertIn("canonicalFeatureStack(next)", html)
            self.assertIn("if(current.technique!=='experimental')return[];", html)
            self.assertIn("zoom===1?'100% / 1:1 canvas'", html)
            self.assertIn(
                "viewer-compare.pixel-mode img{max-width:none;max-height:none;object-fit:contain",
                html,
            )
            self.assertNotIn("(zoom===0?'Fit to view':'100% / 1:1 canvas')", html)
            self.assertNotIn(
                "const next=[...new Set(active?current.featureStack.filter(x=>x!==feature):[...current.featureStack,feature])].sort()",
                html,
            )

            single_file = root / "review-single.html"
            embed_result = subprocess.run(
                [
                    "node",
                    "tools/embed_review_harness.mjs",
                    str(output_html),
                    str(single_file),
                ],
                check=True,
                cwd=Path(__file__).resolve().parents[1],
                capture_output=True,
                text=True,
            )
            self.assertIn("image/png", embed_result.stdout)
            self.assertNotIn("lossless WebP", embed_result.stdout)
            single_html = single_file.read_text(encoding="utf-8")
            embedded_data_match = re.search(
                r"const embeddedAssetData = Object\.freeze\((\{[\s\S]*?\})\);",
                single_html,
            )
            self.assertIsNotNone(embedded_data_match)
            embedded_data = json.loads(embedded_data_match.group(1))
            embedded_alias_match = re.search(
                r"const embeddedAssets = Object\.freeze\((\{[\s\S]*?\})\);",
                single_html,
            )
            self.assertIsNotNone(embedded_alias_match)
            aliases = dict(
                re.findall(
                    r'"([^"]+)":embeddedAssetData\["([^"]+)"\]',
                    embedded_alias_match.group(1),
                )
            )
            self.assertEqual(set(aliases), {asset["assetName"] for asset in manifest})
            self.assertTrue(all(value.startswith("data:image/") for value in embedded_data.values()))
            self.assertTrue(all(reference in embedded_data for reference in aliases.values()))
            self.assertLess(len(embedded_data), len(aliases))
            self.assertIn("embeddedAssetData", single_html)
            self.assertIn("const externalAssets = Object.freeze([]);", single_html)
            self.assertIn("embeddedAssets[asset.assetName]??asset.assetPath", single_html)

            embed_source = (Path(__file__).resolve().parents[1] / "tools/embed_review_harness.mjs").read_text(
                encoding="utf-8"
            )
            self.assertIn("writeEmbeddedAssetObject", embed_source)
            self.assertNotIn("JSON.stringify(embeddedAssets)", embed_source)
            self.assertIn("TFORGE_REVIEW_MAX_MIB", embed_source)

            browser_verifier = (Path(__file__).resolve().parents[1] / "tools/verify_review_harness_browser.py").read_text(
                encoding="utf-8"
            )
            self.assertIn("controlStructure", browser_verifier)
            self.assertIn("selectionRestored", browser_verifier)


if __name__ == "__main__":
    unittest.main()
