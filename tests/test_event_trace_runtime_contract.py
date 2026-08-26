"""Failing-first contract checks for authoritative runtime event evidence."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAYBACK = ROOT / "src/core/PlaybackEngine.cpp"


class EventTraceRuntimeContractTests(unittest.TestCase):
    """Lock the player-side event trace before capture tooling consumes it."""

    def test_player_declares_authoritative_event_trace_contract(self) -> None:
        source = PLAYBACK.read_text(encoding="utf-8")
        for marker in (
            "TFORGE_FSR4_DUMP_EVENT_TRACE",
            "TFORGE_FSR4_DUMP_EVENT_DIR",
            "temporal_forge.event_trace.v1",
            "eventIndex",
            "resetCause",
            "ghostCause",
            "detectorInputs",
            "thresholdProvenance",
            "ptsUs",
        ):
            self.assertIn(marker, source, marker)

    def test_event_trace_is_aligned_with_sequence_frame_indices(self) -> None:
        source = PLAYBACK.read_text(encoding="utf-8")
        self.assertIn("dumpEventTraceFrame", source)
        self.assertIn("fsr4SequenceDumpCount_", source)
        self.assertIn("eventFrameIndex", source)
        self.assertIn("transitionIndex", source)
        self.assertIn('<< "    \\\"reactiveAverage\\\": " << sideInputs.reactiveAverage', source)


if __name__ == "__main__":
    unittest.main()
