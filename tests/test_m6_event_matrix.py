"""Tests for additive M6 event-evidence assembly."""

from __future__ import annotations

import copy
import json
import tempfile
import unittest
from pathlib import Path


class M6EventMatrixTests(unittest.TestCase):
    def _campaign(self) -> dict:
        return {
            "schemaVersion": 2,
            "campaignId": "fixture",
            "baselineCandidateId": "candidate",
            "corpus": {"manifestPath": "manifest.csv", "selection": ["sintel_cave"]},
            "dimensions": {"source": "426x240", "output": "1920x1080"},
            "frame": 48,
            "quality": "high",
            "classes": ["low-light-shadow-detail"],
            "classSelections": {"sintel_cave": ["low-light-shadow-detail"]},
            "requiredMetrics": [
                "psnr_db", "ssim", "edge_ssim", "static_flicker", "edge_variance",
                "motion_compensated_error", "ghost_duration_frames", "reset_recovery_frames",
            ],
            "temporalEvidence": {"complete": False, "status": "pending", "rows": []},
            "candidates": [{
                "id": "candidate",
                "configPath": "config/candidate.json",
                "configSha256": "b" * 64,
                "binarySha256": "a" * 64,
                "gitCommit": "1234567",
                "dimensions": {"source": "426x240", "output": "1920x1080"},
                "reviewAssets": [{"scene": "sintel_cave", "frame": 48, "path": "asset.png", "width": 1920, "height": 1080}],
            }],
        }

    def _event_input(self, root: Path, *, candidate: str = "candidate") -> dict:
        csv_path = root / "metrics.csv"
        csv_path.write_text(
            "candidateId,scene,configId,startFrame,endFrame,class,frames,width,height,static_flicker,edge_variance,motion_compensated_error,ghost_duration_frames,reset_recovery_frames\n"
            f"{candidate},sintel_cave,config/candidate.json,6,23,raw_class,18,1920,1080,0.1,0.2,0.3,0,0\n",
            encoding="utf-8",
        )
        trace = {
            "schema": "temporal_forge.event_trace.v1",
            "identity": {"candidateId": candidate, "scene": "sintel_cave", "configId": "config/candidate.json", "startFrame": 6, "endFrame": 23, "outputWidth": 1920, "outputHeight": 1080, "sourcePath": "input.mp4", "referencePath": "reference.mkv"},
            "capture": {"frames": 18},
            "authoritativeEventIndex": 5,
            "eventFrameIndex": 5,
            "eventTransitionIndex": 4,
            "resetIndex": 4,
            "ghostEventIndex": 5,
            "ghostThreshold": 0.02,
            "resetThreshold": 0.02,
            "authoritativeEvent": {"eventIndex": 5, "detectorSceneCut": True},
            "frames": [{} for _ in range(18)],
        }
        trace_path = root / "events.json"
        trace_path.write_text(json.dumps(trace), encoding="utf-8")
        return {
            "temporalCsv": csv_path,
            "eventTrace": trace_path,
            "motionJson": None,
            "staticMaskJson": None,
            "declaredClass": "low-light-shadow-detail",
        }

    def _base_matrix(self, *, duplicate: bool = False) -> dict:
        rows = [{
            "candidateId": "candidate",
            "scene": "sintel_cave",
            "qualityClass": "low-light-shadow-detail",
            "sequence": {"frames": 8},
            "marker": index,
        } for index in range(20)]
        if not duplicate:
            rows = [rows[0]]
            rows[0]["candidateId"] = "candidate"
        return {
            "schemaVersion": 2,
            "campaignId": "fixture",
            "matrixType": "combined",
            "temporal": rows,
        }

    def test_event_evidence_is_additive_and_preserves_eight_frame_rows(self) -> None:
        from benchmarks.quality_sweeps.event_matrix import assemble_event_matrix

        campaign = {
            "schemaVersion": 2,
            "campaignId": "fixture",
            "baselineCandidateId": "candidate",
            "corpus": {"manifestPath": "manifest.csv", "selection": ["sintel_cave"]},
            "dimensions": {"source": "426x240", "output": "1920x1080"},
            "frame": 48,
            "quality": "high",
            "classes": ["low-light-shadow-detail"],
            "classSelections": {"sintel_cave": ["low-light-shadow-detail"]},
            "requiredMetrics": [
                "psnr_db", "ssim", "edge_ssim", "static_flicker", "edge_variance",
                "motion_compensated_error", "ghost_duration_frames", "reset_recovery_frames",
            ],
            "temporalEvidence": {"complete": False, "status": "pending", "rows": []},
            "candidates": [{
                "id": "candidate",
                "configPath": "config/candidate.json",
                "configSha256": "b" * 64,
                "binarySha256": "a" * 64,
                "gitCommit": "1234567",
                "dimensions": {"source": "426x240", "output": "1920x1080"},
                "reviewAssets": [{"scene": "sintel_cave", "frame": 48, "path": "asset.png", "width": 1920, "height": 1080}],
            }],
        }
        base_rows = [{"candidateId": "candidate", "scene": "sintel_cave", "qualityClass": "low-light-shadow-detail", "sequence": {"frames": 8}}]
        base = {"schemaVersion": 2, "campaignId": "fixture", "matrixType": "combined", "temporal": base_rows}

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            csv_path = root / "metrics.csv"
            csv_path.write_text(
                "candidateId,scene,configId,startFrame,endFrame,class,frames,width,height,static_flicker,edge_variance,motion_compensated_error,ghost_duration_frames,reset_recovery_frames\n"
                "candidate,sintel_cave,config/candidate.json,6,23,raw_class,18,1920,1080,0.1,0.2,0.3,0,0\n",
                encoding="utf-8",
            )
            trace = {
                "schema": "temporal_forge.event_trace.v1",
                "identity": {"candidateId": "candidate", "scene": "sintel_cave", "configId": "config/candidate.json", "startFrame": 6, "endFrame": 23, "outputWidth": 1920, "outputHeight": 1080, "sourcePath": "input.mp4", "referencePath": "reference.mkv"},
                "capture": {"frames": 18},
                "authoritativeEventIndex": 5,
                "eventFrameIndex": 5,
                "eventTransitionIndex": 4,
                "resetIndex": 4,
                "ghostEventIndex": 5,
                "ghostThreshold": 0.02,
                "resetThreshold": 0.02,
                "authoritativeEvent": {"eventIndex": 5, "detectorSceneCut": True},
                "frames": [{} for _ in range(18)],
            }
            trace_path = root / "events.json"
            trace_path.write_text(json.dumps(trace), encoding="utf-8")
            motion_path = root / "motion.json"
            motion_path.write_text(json.dumps({"schema": "temporal_forge.codec_motion.v1", "sourceWidth": 426, "sourceHeight": 240, "targetWidth": 1920, "targetHeight": 1080, "frames": [{} for _ in range(18)]}), encoding="utf-8")
            mask_path = root / "mask.json"
            mask_path.write_text(json.dumps({"schema": "temporal_forge.static_mask.v1", "width": 1920, "height": 1080, "scene": "sintel_cave", "candidateId": "candidate", "frameRange": {"start": 6, "end": 23}}), encoding="utf-8")
            result = assemble_event_matrix(campaign, base, [{"temporalCsv": csv_path, "eventTrace": trace_path, "motionJson": motion_path, "staticMaskJson": mask_path, "declaredClass": "low-light-shadow-detail"}])

        self.assertEqual(len(result["temporal"]), 1)
        self.assertTrue(all(row["sequence"]["frames"] == 8 for row in result["temporal"]))
        self.assertTrue(result["eventEvidence"]["complete"])
        self.assertEqual(result["eventEvidence"]["rows"][0]["sequence"]["frames"], 18)
        self.assertEqual(result["eventEvidence"]["rows"][0]["metricSource"]["rawClass"], "raw_class")

    def test_event_evidence_is_explicitly_non_strict_and_does_not_promote_base_rows(self) -> None:
        from benchmarks.quality_sweeps.event_matrix import assemble_event_matrix

        campaign = self._campaign()
        base = self._base_matrix()
        with tempfile.TemporaryDirectory() as directory:
            result = assemble_event_matrix(campaign, base, [self._event_input(Path(directory))])

        self.assertEqual(result["temporal"][0]["sequence"]["frames"], 8)
        self.assertEqual(result["eventEvidence"]["scope"], "separate_event_evidence")
        self.assertFalse(result["eventEvidence"]["strictMatrixInput"])
        self.assertFalse(result["eventEvidence"]["promotesBaseRows"])
        self.assertEqual(result["eventEvidence"]["coverageKeyFields"], ["candidateId", "scene", "qualityClass"])

    def test_base_rows_must_be_one_row_per_campaign_key_not_just_twenty_rows(self) -> None:
        from benchmarks.quality_sweeps.event_matrix import TemporalMatrixError, assemble_event_matrix

        campaign = self._campaign()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(TemporalMatrixError):
                assemble_event_matrix(campaign, self._base_matrix(duplicate=True), [self._event_input(Path(directory))])

    def test_separate_event_rows_cannot_populate_base_event_metrics(self) -> None:
        from benchmarks.quality_sweeps.event_matrix import TemporalMatrixError, assemble_event_matrix

        campaign = self._campaign()
        base = self._base_matrix()
        base["temporal"][0]["metrics"] = {"ghost_duration_frames": 0}
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(TemporalMatrixError):
                assemble_event_matrix(campaign, base, [self._event_input(Path(directory))])


if __name__ == "__main__":
    unittest.main()
