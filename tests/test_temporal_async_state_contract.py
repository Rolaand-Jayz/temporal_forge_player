"""Contract test for causal temporal state ownership.

The async single-pass path uses two complete uploader objects. Each uploader
owns its own history and recurrent images, so alternating them cannot preserve
the immediately previous temporal state. This test is intentionally written
before the production guard: temporal-state-enabled playback must not select
that path until state is shared or dispatches are serialized.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PLAYBACK = ROOT / "src/core/PlaybackEngine.cpp"


def test_async_temporal_slots_are_not_selected_with_persistent_state():
    source = PLAYBACK.read_text(encoding="utf-8")
    state_marker = "const bool temporalStateEnabled ="
    state_start = source.index(state_marker)
    state_expression = source[state_start : source.index(";", state_start) + 1]
    async_marker = "const bool asyncSlots ="
    async_start = source.index(async_marker)
    async_expression = source[async_start : source.index(";", async_start) + 1]

    assert "TFORGE_FSR4_ENABLE_COLOR_HISTORY" in state_expression
    assert "TFORGE_FSR4_ENABLE_RECURRENT" in state_expression
    assert "!temporalStateEnabled" in async_expression


def test_async_temporal_slots_are_not_selected_with_synthetic_jitter():
    """Jitter phase must commit only after the frame it belongs to publishes."""
    source = PLAYBACK.read_text(encoding="utf-8")
    assert "static const bool syntheticJitterApplied =" in source
    async_marker = "const bool asyncSlots ="
    async_start = source.index(async_marker)
    async_expression = source[async_start : source.index(";", async_start) + 1]

    assert "const bool syntheticJitterEnabled =" in source
    assert "!temporalStateEnabled" in async_expression
    assert "!syntheticJitterEnabled" in async_expression
