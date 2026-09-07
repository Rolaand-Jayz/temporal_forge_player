"""Contract tests for the stateful FSR-to-presentation submission seam.

The tests intentionally inspect the source contract rather than pretending a
headless unit test can prove Vulkan command ordering.  Runtime validation still
has to compare pixels and history publication; these checks prevent the
integration from silently falling back to two submissions or reusing the
upload EASU descriptor set while a prefix command buffer is recorded.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HARNESS_HPP = ROOT / "src/render/Fsr4DispatchHarness.hpp"
HARNESS_CPP = ROOT / "src/render/Fsr4DispatchHarness.cpp"
UPLOADER_HPP = ROOT / "src/render/GpuImageUploader.hpp"
UPLOADER_CPP = ROOT / "src/render/GpuImageUploader.cpp"
PLAYBACK_CPP = ROOT / "src/core/PlaybackEngine.cpp"


class StatefulPresentationFusionContractTests(unittest.TestCase):
    def test_frame_input_exposes_record_only_presentation_hook(self):
        source = HARNESS_HPP.read_text()
        self.assertIn("appendPresentation", source)

    def test_uploader_has_prepare_and_record_only_presentation_api(self):
        header = UPLOADER_HPP.read_text()
        implementation = UPLOADER_CPP.read_text()
        self.assertIn("preparePresentationScaler", header)
        self.assertIn("recordPresentationScaler", header)
        self.assertIn("GpuImageUploader::preparePresentationScaler", implementation)
        self.assertIn("GpuImageUploader::recordPresentationScaler", implementation)

    def test_fsr_records_presentation_before_submitting_its_fence(self):
        source = HARNESS_CPP.read_text()
        record = source.index("in.appendPresentation")
        end_command = source.index("vkEndCommandBuffer(cmd_)", record)
        submit = source.index("vkQueueSubmit(queue_", end_command)
        self.assertLess(record, end_command)
        self.assertLess(end_command, submit)

    def test_playback_prepares_fused_presentation_and_skips_second_submit(self):
        source = PLAYBACK_CPP.read_text()
        self.assertIn("preparePresentationScaler", source)
        self.assertIn("appendPresentation", source)
        self.assertIn("presentationFused", source)
        self.assertIn("TFORGE_FSR4_DISABLE_FUSED_PRESENTATION", source)

    def test_upload_easu_descriptor_set_is_not_reused_for_fused_recording(self):
        source = UPLOADER_CPP.read_text()
        self.assertIn("presentationSet_", source)


if __name__ == "__main__":
    unittest.main()
