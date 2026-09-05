"""Contract tests for capture-time event-trace assembly."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path


class EventTraceAssemblyTests(unittest.TestCase):
    def _record(self, index: int, *, detector: bool = False) -> dict:
        return {
            "schema": "temporal_forge.event_trace.v1",
            "eventIndex": index,
            "eventFrameIndex": index,
            "decoderReceiveIndex": index,
            "transitionIndex": None if index == 0 else index - 1,
            "ptsUs": index * 33333,
            "ptsDeltaMs": 0.0 if index == 0 else 33.333,
            "reset": index == 0 or detector,
            "forcedReset": index == 0,
            "detectorSceneCut": detector,
            "resetCause": "detector_scene_cut" if detector else (
                "forced_reset" if index == 0 else "none"
            ),
            "ghostCause": "detector_scene_cut" if detector else (
                "forced_reset" if index == 0 else "none"
            ),
            "detectorInputs": {
                "histogramDelta": 0.9 if detector else 0.0,
                "avgLumaDelta": 0.4 if detector else 0.0,
                "motionConfidence": 0.2,
                "ptsGapMs": 33.333,
                "expectedFrameIntervalMs": 33.333,
            },
            "thresholdProvenance": {
                "contract": "side_buffer_scene_cut.v1",
                "implementation": "SideBufferSynth::shouldReset",
                "histogramDeltaGreaterThan": 0.65,
                "motionConfidenceLessThan": 0.15,
                "ptsGapMultiplierGreaterThan": 2.5,
            },
            "event": index == 0 or detector,
        }

    def test_assembles_identity_and_only_a_later_detector_event(self) -> None:
        from tools.assemble_event_trace import assemble_event_trace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            for index in range(5):
                (records / f"event_trace_{index:04d}.json").write_text(
                    json.dumps(self._record(index, detector=index == 2)),
                    encoding="utf-8",
                )
            output = root / "events.json"
            document = assemble_event_trace(
                records,
                expected_frames=5,
                candidate_id="base_only_bilinear",
                scene="sintel_cave",
                config_id="stage_a/base_only_bilinear.json",
                start_frame=48,
                source_path="clips/input.mp4",
                reference_path="references/reference.mkv",
                output_width=1920,
                output_height=1080,
                ghost_threshold=0.02,
                reset_threshold=0.02,
            )
            output.write_text(json.dumps(document), encoding="utf-8")

            self.assertEqual(document["identity"]["candidateId"], "base_only_bilinear")
            self.assertEqual(document["identity"]["scene"], "sintel_cave")
            self.assertEqual(document["identity"]["configId"], "stage_a/base_only_bilinear.json")
            self.assertEqual(document["authoritativeEvent"]["eventIndex"], 2)
            self.assertEqual(document["resetIndex"], 1)
            self.assertEqual(document["ghostEventIndex"], 2)
            self.assertEqual(document["ghostThreshold"], 0.02)
            self.assertEqual(document["resetThreshold"], 0.02)
            self.assertEqual(len(document["frames"]), 5)

    def test_rejects_non_contiguous_decoder_receive_order(self) -> None:
        from tools.assemble_event_trace import assemble_event_trace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            for index in range(5):
                record = self._record(index)
                record["decoderReceiveIndex"] = index + (1 if index >= 2 else 0)
                (records / f"event_trace_{index:04d}.json").write_text(
                    json.dumps(record), encoding="utf-8"
                )
            with self.assertRaisesRegex(ValueError, "decoder receive indices"):
                assemble_event_trace(
                    records,
                    expected_frames=5,
                    candidate_id="base_only_bilinear",
                    scene="sintel_cave",
                    config_id="stage_a/base_only_bilinear.json",
                    start_frame=48,
                    source_path="clips/input.mp4",
                    reference_path="references/reference.mkv",
                    output_width=1920,
                    output_height=1080,
                    ghost_threshold=0.02,
                    reset_threshold=0.02,
                )

    def test_selects_later_recorded_event_when_first_event_lacks_context(self) -> None:
        from tools.assemble_event_trace import assemble_event_trace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            for index in range(18):
                record = self._record(index, detector=index in (1, 5))
                record["ptsUs"] = round(index * 125000 / 3)
                if index == 5:
                    record["ptsDeltaMs"] = 41.666
                    record["resetCause"] = "detector_scene_cut"
                    record["ghostCause"] = "detector_scene_cut"
                    record["detectorInputs"] = {
                        "histogramDelta": 0.668208122,
                        "avgLumaDelta": 0.0803499669,
                        "motionConfidence": 0.310404599,
                        "ptsGapMs": 41.6660004,
                        "expectedFrameIntervalMs": 23.4417,
                    }
                    record["thresholdProvenance"] = {
                        "contract": "side_buffer_scene_cut.v1",
                        "implementation": "SideBufferSynth::shouldReset",
                        "histogramDeltaGreaterThan": 0.65,
                        "motionConfidenceLessThan": 0.15,
                        "ptsGapMultiplierGreaterThan": 2.5,
                    }
                (records / f"event_trace_{index:04d}.json").write_text(
                    json.dumps(record), encoding="utf-8"
                )

            document = assemble_event_trace(
                records,
                expected_frames=18,
                candidate_id="base_only_bilinear",
                scene="sintel_cave",
                config_id="stage_a/base_only_bilinear.json",
                start_frame=6,
                source_path="clips/input.mp4",
                reference_path="references/reference.mkv",
                output_width=1920,
                output_height=1080,
                ghost_threshold=0.02,
                reset_threshold=0.02,
            )

            selected = json.loads(
                (records / "event_trace_0005.json").read_text(encoding="utf-8")
            )
            self.assertEqual(document["authoritativeEventIndex"], 5)
            self.assertEqual(document["eventTransitionIndex"], 4)
            self.assertEqual([event["eventIndex"] for event in document["events"]], [1, 5])
            self.assertEqual(document["authoritativeEvent"], selected)
            self.assertEqual(document["resetCause"], "detector_scene_cut")
            self.assertEqual(document["ghostCause"], "detector_scene_cut")
            self.assertEqual(document["authoritativeEvent"]["ptsUs"], 208333)
            self.assertEqual(
                document["authoritativeEvent"]["detectorInputs"],
                selected["detectorInputs"],
            )
            self.assertEqual(
                document["authoritativeEvent"]["thresholdProvenance"],
                selected["thresholdProvenance"],
            )
            self.assertEqual(document["authoritativeCause"]["cause"], "histogram_delta")
            self.assertEqual(document["authoritativeCause"]["signals"], ["histogram_delta"])

    def test_prefers_eligible_pts_gap_and_preserves_raw_detector_labels(self) -> None:
        from tools.assemble_event_trace import assemble_event_trace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            for index in range(18):
                detector = index in (3, 4)
                record = self._record(index, detector=detector)
                if index == 3:
                    record["detectorInputs"]["motionConfidence"] = 0.0
                if index == 4:
                    record["ptsUs"] = 667000
                    record["ptsDeltaMs"] = 542.0
                    record["detectorInputs"] = {
                        "histogramDelta": 0.324228227,
                        "avgLumaDelta": 0.0772547424,
                        "motionConfidence": 0.315183222,
                        "ptsGapMs": 542.0,
                        "expectedFrameIntervalMs": 21.3900261,
                    }
                elif index > 4:
                    record["ptsUs"] = 667000 + (index - 4) * 33333
                (records / f"event_trace_{index:04d}.json").write_text(
                    json.dumps(record), encoding="utf-8"
                )

            document = assemble_event_trace(
                records,
                expected_frames=18,
                candidate_id="base_only_bilinear",
                scene="tos_daylight",
                quality_class="fine-fabric-texture",
                analysis_frame_indices=[0, 1, 2, 3],
                config_id="stage_a/base_only_bilinear.json",
                start_frame=198,
                source_path="clips/tos_daylight_event.mp4",
                reference_path="references/tos_daylight_event.mkv",
                output_width=1920,
                output_height=1080,
                ghost_threshold=0.02,
                reset_threshold=0.02,
            )

            self.assertEqual(document["authoritativeEventIndex"], 4)
            self.assertEqual(document["authoritativeEvent"]["eventIndex"], 4)
            self.assertEqual(document["authoritativeEvent"]["detectorSceneCut"], True)
            self.assertEqual(document["authoritativeEvent"]["resetCause"], "detector_scene_cut")
            self.assertEqual(document["authoritativeEvent"]["ghostCause"], "detector_scene_cut")
            self.assertEqual(document["authoritativeCause"]["cause"], "pts_gap")
            self.assertEqual(document["authoritativeCause"]["signals"], ["pts_gap"])
            self.assertEqual(document["authoritativeCause"]["ptsUs"], 667000)
            self.assertEqual(document["authoritativeCause"]["ptsDeltaMs"], 542.0)
            self.assertEqual(
                document["authoritativeCause"]["detectorInputs"]["histogramDelta"],
                0.324228227,
            )
            self.assertEqual(
                document["authoritativeCause"]["thresholdProvenance"]["ptsGapMultiplierGreaterThan"],
                2.5,
            )
            self.assertGreater(
                document["authoritativeCause"]["detectorInputs"]["ptsGapMs"],
                document["authoritativeCause"]["detectorInputs"]["expectedFrameIntervalMs"]
                * document["authoritativeCause"]["thresholdProvenance"]["ptsGapMultiplierGreaterThan"],
            )

    def test_rejects_incomplete_or_initialization_only_records(self) -> None:
        from tools.assemble_event_trace import assemble_event_trace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            (records / "event_trace_0000.json").write_text(
                json.dumps(self._record(0)), encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "complete event trace"):
                assemble_event_trace(
                    records,
                    expected_frames=1,
                    candidate_id="base_only_bilinear",
                    scene="sintel_cave",
                    config_id="config.json",
                    start_frame=48,
                    source_path="input.mp4",
                    reference_path="reference.mkv",
                    output_width=1920,
                    output_height=1080,
                    ghost_threshold=0.02,
                    reset_threshold=0.02,
                )

    def test_binds_quality_class_and_explicit_static_analysis_window(self) -> None:
        from tools.assemble_event_trace import assemble_event_trace

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            for index in range(7):
                (records / f"event_trace_{index:04d}.json").write_text(
                    json.dumps(self._record(index, detector=index == 4)),
                    encoding="utf-8",
                )

            document = assemble_event_trace(
                records,
                expected_frames=7,
                candidate_id="base_only_bilinear",
                scene="tos_daylight",
                quality_class="fine-fabric-texture",
                analysis_frame_indices=[0, 1, 2, 3],
                config_id="stage_a/base_only_bilinear.json",
                start_frame=198,
                source_path="clips/tos_daylight_event.mp4",
                reference_path="references/tos_daylight_event.mkv",
                output_width=1920,
                output_height=1080,
                ghost_threshold=0.02,
                reset_threshold=0.02,
            )

            self.assertEqual(
                document["identity"]["qualityClass"], "fine-fabric-texture"
            )
            self.assertEqual(document["analysisFrameIndices"], [0, 1, 2, 3])


if __name__ == "__main__":
    unittest.main()
