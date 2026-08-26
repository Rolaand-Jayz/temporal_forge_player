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
                "tos_daylight_640x360_high_crf12_f48.png",
                "tos_daylight_640x360_high_crf12_f48_cross_control.png",
                "tos_daylight_640x360_high_crf12_f48_cas0p00.png",
                "tos_daylight_640x360_high_crf12_f48_cas0p02.png",
                "tos_daylight_640x360_high_crf12_f48_cross_direct_unjittered.png",
                "tos_daylight_640x360_high_crf12_f48_cross_jitter_off.png",
                "tos_daylight_reference_1280x720_f48.png",
                "tos_daylight_input854x480_to1280x720_lanczos.png",
                "tos_daylight_input854x480_to1280x720_candidate-1.png",
                "tos_daylight_input854x480_to1280x720_candidate-2.png",
                "tos_daylight_input854x480_to1280x720_candidate-3.png",
                "tos_daylight_input854x480_to1280x720_candidate-4.png",
                "tos_daylight_input854x480_to1280x720_candidate-5.png",
                "tos_daylight_input854x480_to1280x720_candidate-6.png",
                "tos_daylight_input854x480_to1280x720_opt-in-1.png",
                "tos_daylight_input1280x720_to1920x1080_bicubic.png",
                "tos_daylight_input1920x1080_to3840x2160_temporal-forge.png",
                "synthetic_motion_426x240_to1920x1080_temporal_forge_review.png",
            )
            for name in names:
                dimensions = (1280, 720)
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
            self.assertTrue(
                {"native", "temporal-forge", "candidate-1", "opt-in-1", "opt-in-2", "opt-in-3", "opt-in-4"}
                <= techniques
            )
            contextual_reference = next(asset for asset in manifest if asset["technique"] == "native")
            self.assertEqual(contextual_reference["inputResolution"], "640x360")
            self.assertEqual(contextual_reference["outputResolution"], "1280x720")
            candidate = next(asset for asset in manifest if asset["technique"] == "candidate-1")
            self.assertEqual(candidate["experimentId"], "candidate-1")
            tier_candidate = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_candidate-1.png"))
            self.assertEqual(tier_candidate["technique"], "candidate-1")
            tier_candidate_two = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_candidate-2.png"))
            self.assertEqual(tier_candidate_two["technique"], "candidate-2")
            tier_candidate_three = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_candidate-3.png"))
            self.assertEqual(tier_candidate_three["technique"], "candidate-3")
            tier_candidate_four = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_candidate-4.png"))
            self.assertEqual(tier_candidate_four["technique"], "candidate-4")
            tier_candidate_five = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_candidate-5.png"))
            self.assertEqual(tier_candidate_five["technique"], "candidate-5")
            tier_candidate_six = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_candidate-6.png"))
            self.assertEqual(tier_candidate_six["technique"], "candidate-6")
            tier_opt_in = next(asset for asset in manifest if asset["assetName"].endswith("input854x480_to1280x720_opt-in-1.png"))
            self.assertEqual(tier_opt_in["technique"], "opt-in-1")
            legacy_assets = [asset for asset in manifest if asset["assetName"].startswith("tos_daylight_640x360") or asset["assetName"].startswith("sintel_")]
            self.assertTrue(all(asset["inputResolution"] == "640x360" for asset in legacy_assets))
            self.assertTrue(all(asset["outputResolution"] == "1280x720" for asset in legacy_assets))
            self.assertNotIn("Candidate #1", html.split("const assetManifest", 1)[0])
            self.assertIn("'candidate-1':'Candidate #1'", html)
            self.assertIn("'temporal-forge':'Temporal Forge'", html)
            self.assertIn("'native':'Native reference'", html)
            self.assertIn("'opt-in-1':'Opt-in #1'", html)
            self.assertIn("'opt-in-4':'Opt-in #4'", html)
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
            self.assertIn("function intendedCanvas(left,right)", html)
            self.assertIn("shared canvas ", html)
            self.assertIn("actual pixels", html)
            self.assertIn("zoom===1?'100% / 1:1 highest canvas'", html)
            self.assertIn(
                "viewer-compare.pixel-mode img{max-width:none;max-height:none;object-fit:contain",
                html,
            )
            self.assertIn("stage.addEventListener('wheel'", html)
            self.assertIn("id=\"lens-toggle\"", html)
            self.assertIn("function setLensEnabled", html)
            self.assertIn("e.ctrlKey", html)
            self.assertIn("className='magnifier'", html)
            self.assertIn("zoom=Math.max(.25,Math.min(16,zoom))", html)
            self.assertIn("'lanczos':'Lanczos'", html)
            self.assertIn("'bicubic':'Bicubic'", html)
            self.assertIn("native-input", html)
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
                    r'"([^"]+)":"([^"]+)"',
                    embedded_alias_match.group(1),
                )
            )
            self.assertEqual(set(aliases), {asset["assetName"] for asset in manifest})
            self.assertTrue(all(value in {"image/png", "image/webp"} for value in embedded_data.values()))
            self.assertTrue(all(reference in embedded_data for reference in aliases.values()))
            self.assertLess(len(embedded_data), len(aliases))
            self.assertIn("embeddedAssetData", single_html)
            self.assertIn("const externalAssets = Object.freeze([]);", single_html)
            self.assertIn("function embeddedAssetSource", single_html)

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
