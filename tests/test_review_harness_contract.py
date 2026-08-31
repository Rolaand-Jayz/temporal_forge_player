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

    def test_native_controls_cover_every_declared_input_output_pair(self) -> None:
        """Native input/reference must remain available across the full review matrix."""

        repo = Path(__file__).resolve().parents[1]
        matrix = json.loads(
            (repo / "benchmarks/video_corpus/review_best_finds.json").read_text(encoding="utf-8")
        )
        review_root = repo / "benchmarks/video_corpus/results/review_best_finds_500"
        scenes = matrix["scenes"]
        inputs = matrix["inputs"]
        outputs = matrix["outputs"]

        for arm in ("native-input",):
            names = {
                path.name
                for path in review_root.glob(f"*_{arm}.png")
            }
            expected = {
                f"{scene}_input{input_resolution}_to{output_resolution}_{arm}.png"
                for scene in scenes
                for input_resolution in inputs
                for output_resolution in outputs
            }
            self.assertEqual(
                names,
                expected,
                f"{arm} must cover every declared scene/input/output combination",
            )

    def test_builder_emits_structured_real_asset_manifest_and_split_controls(self) -> None:
        review_matrix = json.loads(
            (Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json").read_text(
                encoding="utf-8"
            )
        )
        review_arm_ids = {arm["id"] for arm in review_matrix["arms"]}
        self.assertEqual(
            review_arm_ids,
            {
                "native-input",
                "native-output",
                "pre-campaign-temporal-forge", "lanczos", "fsr1", "best-findings-temporal",
                "fsr1-best-findings-temporal", "best-findings-temporal-synthetic-jitter",
            },
        )
        self.assertEqual(review_matrix["inputs"], ["426x240", "640x360", "854x480", "1280x720", "1920x1080"])
        self.assertEqual(review_matrix["outputs"], ["854x480", "1280x720", "1920x1080", "2560x1440", "3840x2160"])
        self.assertEqual(len(review_matrix["outputs"]), 5)
        capture_script = (Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/capture_review_best_finds.sh").read_text(
            encoding="utf-8"
        )
        review_config = json.loads(
            (Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_current_path.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertFalse(review_config["qualityLab"]["enabled"])
        self.assertEqual(review_config["qualityLab"]["composition"]["mode"], "current")
        self.assertIn('TFORGE_QUALITY_LAB_CONFIG="$review_config"', capture_script)
        self.assertIn("TFORGE_REVIEW_FORCE_RECAPTURE", capture_script)
        self.assertIn("source provenance mismatch", capture_script)
        self.assertIn('arm_id" == "lanczos"', capture_script)
        self.assertIn('arm_id" == "native-input"', capture_script)
        self.assertIn('arm_id" == "native-reference"', capture_script)
        self.assertIn("name '*_lanczos.png'", capture_script)
        self.assertIn("best-findings-temporal", capture_script)
        self.assertIn("fsr1-best-findings-temporal", capture_script)
        builder_source = (Path(__file__).resolve().parents[1] / "tools/build_review_harness.mjs").read_text(encoding="utf-8")
        self.assertIn("declaredReviewMatrix.arms", builder_source)
        self.assertIn("fsr1-best-findings-temporal", builder_source)
        self.assertIn("a.technique==='native-input'&&f==='outputResolution'", builder_source)
        self.assertIn("a.technique==='native-output'&&f==='inputResolution'", builder_source)
        self.assertIn("if(!values.length)return null", builder_source)
        self.assertIn("if(value==='native-input'||value==='native-output')", builder_source)
        self.assertIn("Technique buttons are a shared pool", builder_source)
        self.assertNotIn('"best-find-1"', (Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json").read_text(encoding="utf-8"))
        self.assertNotIn('"combined-best"', (Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json").read_text(encoding="utf-8"))
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
                "sintel_cave_input640x360_to1920x1080_opt-in-5_dense-flow.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-6_motion-validation.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-7_confidence-map.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-8_recurrent-coordinate.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-9_single-history.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-10_confidence085.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-11_recurrent-reset-only.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-12_recurrent-coordinate-combo.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-13_confidence-map-combo.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-14_phase1-A.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-21_double-history.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-22_combo.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-23_combo.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-24_combo.png",
                "tos_daylight_input640x360_to1920x1080_opt-in-25_combo.png",
                "sintel_rooftop_input640x360_to1920x1080_opt-in-25_combo.png",
                "sintel_cave_input640x360_to1920x1080_opt-in-25_combo.png",
                "tos_daylight_640x360_to1920x1080_temporal-forge_frame06.png",
                "tos_daylight_640x360_to1920x1080_native-reference_frame06.png",
                "tos_daylight_640x360_to1920x1080_lanczos_frame06.png",
                "synthetic_motion_426x240_to1920x1080_temporal_forge_review.png",
            )
            for name in names:
                dimensions = (1280, 720)
                (review_root / name).write_bytes(_png_with_dimensions(*dimensions))
            # The production review batch guarantees both native peers for
            # every declared scene/input/output tuple. Add the complete
            # complete matrix to this tiny fixture so the builder's coverage
            # guard is exercised instead of bypassed in contract tests.
            for scene in review_matrix["scenes"]:
                for input_resolution in review_matrix["inputs"]:
                    for output_resolution in review_matrix["outputs"]:
                        name = f"{scene}_input{input_resolution}_to{output_resolution}_native-input.png"
                        (review_root / name).write_bytes(_png_with_dimensions(1280, 720))
                for output_resolution in review_matrix["outputs"]:
                    name = f"{scene}_output{output_resolution}_native-output.png"
                    (review_root / name).write_bytes(_png_with_dimensions(1280, 720))

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
            for technique in ("native-input",):
                native_pairs = {
                    (asset["scene"], asset["inputResolution"], asset["outputResolution"])
                    for asset in manifest
                    if asset["technique"] == technique
                }
                self.assertEqual(
                    native_pairs,
                    {
                        (scene, input_resolution, output_resolution)
                        for scene in review_matrix["scenes"]
                        for input_resolution in review_matrix["inputs"]
                        for output_resolution in review_matrix["outputs"]
                    },
                    "fixture must retain the complete native matrix for both mirrored controls",
                )
            self.assertFalse(any("synthetic" in asset["scene"] for asset in manifest))
            self.assertFalse(any(asset["assetName"].endswith("_gpu_raw.png") for asset in manifest))
            techniques = {asset["technique"] for asset in manifest}
            self.assertTrue(
                {"native", "temporal-forge", "candidate-1", "opt-in-1", "opt-in-2", "opt-in-3", "opt-in-4"}
                <= techniques
            )
            contextual_reference = next(asset for asset in manifest if asset["assetName"] == "tos_daylight_reference_1280x720_f48.png")
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
            easy_temporal = next(asset for asset in manifest if asset["assetName"].endswith("640x360_to1920x1080_temporal-forge_frame06.png"))
            self.assertEqual(easy_temporal["inputResolution"], "640x360")
            self.assertEqual(easy_temporal["outputResolution"], "1920x1080")
            self.assertEqual(easy_temporal["technique"], "temporal-forge")
            easy_reference = next(asset for asset in manifest if asset["assetName"].endswith("640x360_to1920x1080_native-reference_frame06.png"))
            self.assertEqual(easy_reference["technique"], "native-reference")
            easy_lanczos = next(asset for asset in manifest if asset["assetName"].endswith("640x360_to1920x1080_lanczos_frame06.png"))
            self.assertEqual(easy_lanczos["technique"], "lanczos")
            legacy_assets = [asset for asset in manifest if asset["assetName"] in {
                "tos_daylight_640x360_high_crf12_f48.png",
                "tos_daylight_640x360_high_crf12_f48_cross_control.png",
                "tos_daylight_640x360_high_crf12_f48_cas0p00.png",
                "tos_daylight_640x360_high_crf12_f48_cas0p02.png",
                "tos_daylight_640x360_high_crf12_f48_cross_direct_unjittered.png",
                "tos_daylight_640x360_high_crf12_f48_cross_jitter_off.png",
            }]
            self.assertTrue(all(asset["inputResolution"] == "640x360" for asset in legacy_assets))
            self.assertTrue(all(asset["outputResolution"] == "1280x720" for asset in legacy_assets))
            self.assertNotIn("Candidate #1", html.split("const assetManifest", 1)[0])
            self.assertIn("'candidate-1':'Candidate #1'", html)
            self.assertIn("'temporal-forge':'Temporal Forge'", html)
            self.assertIn("'pre-campaign-temporal-forge':'Pre-campaign Temporal Forge'", html)
            self.assertIn("'native-reference':'Native reference'", html)
            self.assertIn("'opt-in-1':'Opt-in #1'", html)
            self.assertIn("'opt-in-4':'Opt-in #4'", html)
            self.assertIn("'opt-in-8':'Opt-in #8'", html)
            self.assertIn("'opt-in-13':'Opt-in #13'", html)
            self.assertIn("'opt-in-14':'Opt-in #14'", html)
            self.assertIn("'opt-in-21':'Opt-in #21'", html)
            self.assertIn("'opt-in-22':'Opt-in #22'", html)
            self.assertIn("'opt-in-24':'Opt-in #24'", html)
            self.assertIn("'opt-in-25':'Opt-in #25'", html)
            self.assertIn('const state={left:', html)
            self.assertIn("compare-stage", html)
            self.assertIn("Upscale technique", html)
            # Keep every declared tier visible even when a capture is missing;
            # the availability predicate disables invalid combinations.
            self.assertIn("const declaredInputResolutions = Object.freeze(declaredReviewMatrix.inputs ?? []);", html)
            self.assertIn("const declaredOutputResolutions = Object.freeze(declaredReviewMatrix.outputs ?? []);", html)
            self.assertIn("field==='inputResolution'? [...declaredInputResolutions]", html)
            self.assertIn("field==='outputResolution' ? [...declaredOutputResolutions]", html)
            for resolution in ("426x240", "640x360", "854x480", "1280x720", "1920x1080"):
                self.assertIn(f'"{resolution}"', html)
            for resolution in ("1280x720", "1920x1080", "2560x1440", "3840x2160"):
                self.assertIn(f'"{resolution}"', html)
            self.assertIn("const applicable=modifierFields.filter", html)
            self.assertIn("a[f]===''", html)
            self.assertIn('data-view="pixel"', html)
            self.assertIn("writeHash()", html)
            self.assertIn("featureStackKey", html)
            self.assertIn("const featureOrder=", html)
            self.assertIn("canonicalFeatureStack(next)", html)
            self.assertIn('displayCasStrength', html)
            self.assertIn('Display CAS · 0.20', html)
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

    def test_best_findings_motion_metadata_matches_the_promoted_path(self) -> None:
        """Best-findings review assets must request real codec motion, not empty refinement."""

        manifest_path = Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for arm_id in ("best-findings-temporal", "fsr1-best-findings-temporal", "best-findings-temporal-synthetic-jitter"):
            arm = next(arm for arm in manifest["arms"] if arm["id"] == arm_id)
            self.assertEqual(arm["env"].get("TFORGE_FSR4_MOTION_ESTIMATOR"), "codec")
            self.assertNotIn("TFORGE_FSR4_EXPERIMENTAL_REFINE_MOTION", arm["env"])
            # Missing or unusable codec references are a known video-input
            # failure mode. The combined path may use its conservative global
            # fallback, but it must advertise zero confidence for an empty
            # field so stale history cannot be treated as half-trusted.
            self.assertEqual(
                arm["env"].get("TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING"), "1"
            )
            self.assertEqual(
                arm["env"].get("TFORGE_FSR4_EXPERIMENTAL_EMPTY_MOTION_CONFIDENCE"),
                "0.0",
            )

    def test_best_findings_do_not_bypass_motion_dependent_history(self) -> None:
        """Promoted temporal arms must use the postpass history composition path."""

        manifest_path = Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for arm_id in ("best-findings-temporal", "fsr1-best-findings-temporal", "best-findings-temporal-synthetic-jitter"):
            arm = next(arm for arm in manifest["arms"] if arm["id"] == arm_id)
            self.assertNotIn("TFORGE_FSR4_EXPERIMENTAL_SINGLE_HISTORY_BLEND", arm["env"])

    def test_best_findings_include_per_pixel_history_validation(self) -> None:
        """Combined temporal review arms must carry the measured photometric gate."""

        manifest_path = Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for arm_id in ("best-findings-temporal", "fsr1-best-findings-temporal", "best-findings-temporal-synthetic-jitter"):
            arm = next(arm for arm in manifest["arms"] if arm["id"] == arm_id)
            self.assertEqual(
                arm["env"].get("TFORGE_FSR4_EXPERIMENTAL_PHOTOMETRIC_HISTORY_GATE"),
                "1",
            )

    def test_combined_review_arms_use_the_measured_zero_jitter_policy(self) -> None:
        """Combined temporal review pixels must use the stable jitter policy."""

        manifest_path = Path(__file__).resolve().parents[1] / "benchmarks/video_corpus/review_best_finds.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        for arm_id in ("best-findings-temporal", "fsr1-best-findings-temporal"):
            arm = next(arm for arm in manifest["arms"] if arm["id"] == arm_id)
            self.assertEqual(arm["env"].get("TFORGE_FSR4_JITTER_MODE"), "off")
        jitter_arm = next(
            arm for arm in manifest["arms"]
            if arm["id"] == "best-findings-temporal-synthetic-jitter"
        )
        self.assertNotIn("TFORGE_FSR4_JITTER_MODE", jitter_arm["env"])
        self.assertEqual(
            jitter_arm["env"].get("TFORGE_FSR4_INTEGRATED_BEST_FINDINGS_JITTER"), "1"
        )

    def test_review_capture_pins_single_game_display_cas(self) -> None:
        """Temporal Forge review captures must use the one game display CAS stage."""

        repo = Path(__file__).resolve().parents[1]
        script = (repo / "benchmarks/video_corpus/capture_review_best_finds.sh").read_text(
            encoding="utf-8"
        )
        # Review captures must match the game's display policy exactly: one
        # post-reconstruction CAS pass at .20.  Do not let a shell-level
        # override, inherited disable flag, chained pass, or arm metadata
        # silently change what a reviewer is looking at.
        self.assertIn('TFORGE_FSR4_CAS_STRENGTH="0.20"', script)
        self.assertIn('TFORGE_FSR4_LEGACY_RCAS_STRENGTH="0"', script)
        self.assertIn('TFORGE_FSR4_CHAIN_PASSES="0"', script)
        self.assertIn('env -u TFORGE_FSR4_DISABLE_CAS', script)

    def test_combined_temporal_arms_use_the_combined_motion_config(self) -> None:
        """Best-findings captures must not silently fall back to the spatial config."""

        repo = Path(__file__).resolve().parents[1]
        script = (repo / "benchmarks/video_corpus/capture_review_best_finds.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("review_best_findings_path.json", script)
        self.assertIn(
            "best-findings-temporal|fsr1-best-findings-temporal|best-findings-temporal-synthetic-jitter",
            script,
        )

    def test_combined_temporal_arms_reassert_integrated_findings_after_manifest(self) -> None:
        """Manifest metadata cannot override the combined temporal pipeline."""

        repo = Path(__file__).resolve().parents[1]
        script = (repo / "benchmarks/video_corpus/capture_review_best_finds.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn('TFORGE_FSR4_MOTION_ESTIMATOR="codec"', script)
        self.assertNotIn('TFORGE_FSR4_MOTION_EDGE_AWARE="1"', script)
        self.assertIn('TFORGE_FSR4_MOTION_FALLBACK_AFTER_FILTERING="1"', script)
        self.assertIn('TFORGE_FSR4_INTEGRATED_HISTORY_CONFIDENCE="1"', script)


if __name__ == "__main__":
    unittest.main()
