"""Static contract checks for the portable file-based comparison harness."""

from pathlib import Path
import re
import unittest


class ReviewHarnessContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[1] / "review_harness"
        cls.html = (cls.root / "index.html").read_text(encoding="utf-8")

    def test_portable_minimum_structure(self) -> None:
        self.assertTrue((self.root / "index.html").is_file())
        self.assertTrue((self.root / "images").is_dir())
        self.assertTrue((self.root / "images/no_image.svg").is_file())
        self.assertNotIn("http://", self.html)
        self.assertNotIn("https://", self.html)
        self.assertIn("<style>", self.html)
        self.assertIn("<script>", self.html)

    def test_canonical_filename_constructor_is_single_and_strict(self) -> None:
        self.assertEqual(len(re.findall(r"function canonicalFilename", self.html)), 1)
        self.assertIn("scene-${scene}__frame-${frame}__in-${selection.input}p__method-${selection.method}__out-${selection.output}p${suffix}.png", self.html)
        self.assertIn("window.__tforgeCanonicalFilename = canonicalFilename", self.html)
        self.assertIn("cas20", self.html)

    def test_required_controls_and_methods_are_always_present(self) -> None:
        for value in (360, 480, 540, 720, 1080, 1440, 2160):
            self.assertIn(str(value), self.html)
        for method in ("current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
                       "conventional_lanczos", "conventional_bicubic"):
            self.assertIn(method, self.html)
        self.assertIn("fsr_${value}x_downsample_${placement}", self.html)
        self.assertIn("fsr_nativeaa_downsample_${placement}", self.html)

    def test_harness_uses_four_campaign_scenes(self) -> None:
        for scene in ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave"):
            self.assertIn(scene, self.html)
        self.assertIn("const MULTIPLIERS", self.html)
        self.assertIn("const CAS_PLACEMENTS", self.html)
        self.assertIn("const VIEW_MODES", self.html)
        self.assertIn("['native','Matched source']", self.html)
        self.assertIn("MATCHED SOURCE IS NOT NATIVEAA", self.html)
        for label in ("2×", "2.25×", "2.5×", "2.75×", "3×", "Resolve CAS .20", "External post-reduction CAS .20", "No CAS"):
            self.assertIn(label, self.html)
        self.assertIn("selection.view === 'native'", self.html)
        self.assertIn("'__native'", self.html)

    def test_comparison_integrity_and_missing_asset_behavior(self) -> None:
        for token in ("clip-path", "sweep-input", "panX", "panY", "NO IMAGE", "naturalWidth", "1920", "1080"):
            self.assertIn(token, self.html)
        self.assertNotIn("filter:", self.html)
        self.assertIn("image.src = entry.path", self.html)
        self.assertIn("entry.validation !== 'validated_experiment'", self.html)
        self.assertIn("image.src = 'images/no_image.svg'", self.html)
        self.assertIn("state[side][button.closest('[data-key]').dataset.key]", self.html)

    def test_zoom_uses_one_scrollable_aligned_stage(self) -> None:
        self.assertIn("overflow:auto", self.html)
        self.assertIn('id="comparison-stage"', self.html)
        self.assertIn("$('comparison-stage').style.width", self.html)
        self.assertIn("$('comparison').scrollLeft", self.html)
        self.assertIn("$('comparison').scrollTop", self.html)


if __name__ == "__main__":
    unittest.main()
