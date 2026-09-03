"""Static contract checks for the portable campaign review harness."""

from pathlib import Path
import json
import re
import unittest


class ReviewHarnessContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repo = Path(__file__).resolve().parents[1]
        cls.root = cls.repo / "review_harness"
        cls.html = (cls.root / "index.html").read_text(encoding="utf-8")
        cls.css = (cls.root / "styles.css").read_text(encoding="utf-8")
        cls.js = (cls.root / "app.js").read_text(encoding="utf-8")
        cls.catalog = (cls.root / "catalog.js").read_text(encoding="utf-8")
        cls.plan = json.loads((
            cls.repo / "benchmarks/quality_sweeps/quality_campaign_capture_plan.json"
        ).read_text(encoding="utf-8"))

    def test_portable_structure_has_no_network_dependency(self) -> None:
        for path in ("index.html", "styles.css", "app.js", "catalog.js", "favicon.svg", "images/no_image.svg"):
            self.assertTrue((self.root / path).is_file(), path)
        joined = self.html + self.css + self.js
        self.assertNotIn("http://", joined)
        self.assertNotIn("https://", joined)
        self.assertIn('href="styles.css"', self.html)
        self.assertIn('href="favicon.svg"', self.html)
        self.assertIn('src="app.js"', self.html)

    def test_exact_legal_resolution_matrix_is_shared(self) -> None:
        expected = {
            (360, 480), (360, 720), (360, 1080),
            (480, 720), (480, 1080), (480, 1440),
            (720, 1080), (720, 1440), (720, 2160),
            (1080, 1440), (1080, 2160),
        }
        planned = {(item["inputHeight"], item["outputHeight"]) for item in self.plan["pairs"]}
        fallback = {
            tuple(map(int, match))
            for match in re.findall(r"\[(360|480|720|1080),(480|720|1080|1440|2160)\]", self.js)
        }
        self.assertEqual(planned, expected)
        self.assertEqual(fallback, expected)
        self.assertNotIn("540", self.js + self.catalog)
        self.assertIn("outputsForInput", self.js)

    def test_all_campaign_scenes_and_methods_are_selectable(self) -> None:
        for scene in ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave"):
            self.assertIn(scene, self.js)
        for method in ("current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
                       "conventional_lanczos", "conventional_bicubic"):
            self.assertIn(method, self.js)
        self.assertIn("fsr_${selection.multiplier}x_downsample_${selection.placement}", self.js)
        self.assertIn("fsr_nativeaa_downsample_${selection.placement}", self.js)
        for label in ("CAS 0.20 before downsampling", "CAS 0.20 after downsampling",
                      "No CAS sharpening"):
            self.assertIn(label, self.js + self.catalog)
        self.assertIn('aria-label="Independent downsampling test arms"', self.html)

    def test_canonical_identity_includes_frame_pair_method_and_view(self) -> None:
        self.assertEqual(len(re.findall(r"function canonicalFilename", self.js)), 1)
        self.assertIn("scene-${scene}__frame-${frame}__in-${selection.input}p__method-${selection.method}__out-${selection.output}p${suffix}.png", self.js)
        self.assertIn("window.__tforgeCanonicalFilename=canonicalFilename", self.js)
        self.assertIn("entry.validation!=='validated_experiment'", self.js)
        self.assertIn("images/no_image.svg", self.js)

    def test_comparison_is_aligned_filter_free_and_accessible(self) -> None:
        for token in ("clip-path", "overflow:auto", "comparison-stage", "scrollLeft", "scrollTop"):
            self.assertIn(token, self.css + self.js + self.html)
        self.assertNotIn("filter:", self.css.replace("backdrop-filter:", ""))
        self.assertIn('aria-label="Comparison divider"', self.html)
        self.assertIn("prefers-reduced-motion", self.css)
        self.assertIn(":focus-visible", self.css)
        self.assertIn('label for="${id}"', self.js)
        self.assertIn('role="group"', self.js)

    def test_page_reads_as_a_complete_site_surface(self) -> None:
        for element in ("<header", "<nav", "<main", "<section", "<footer"):
            self.assertIn(element, self.html)
        for section in ('id="viewer"', 'id="matrix"', 'id="evidence"'):
            self.assertIn(section, self.html)
        self.assertIn("Awaiting the new campaign capture", self.js)


if __name__ == "__main__":
    unittest.main()
