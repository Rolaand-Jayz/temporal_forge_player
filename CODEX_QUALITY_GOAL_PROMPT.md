# Temporal Forge — Codex Quality Campaign Launch Prompt

Read `AGENTS.md` and `docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md` in full before modifying code.

Execute the reconstruction-quality plan autonomously.

## Goal

Make Temporal Forge's reconstruction materially better while preserving its existing native Vulkan/RDNA3 real-time performance advantage.

Quality is the sole priority for this campaign.

Do not perform UI work, player feature development, unrelated refactoring, packaging work, or other feature creep.

## Execution rules

Treat `docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md` as a living experiment plan and laboratory log, not a static checklist.

As you work:

1. Establish and preserve the current baseline before changing image behavior.
2. Build the runtime-configurable Quality Lab infrastructure before hardcoding experimental variants.
3. Execute experiments in the plan's staged order.
4. Respect every decision gate.
5. Do not blindly implement later stages when earlier evidence changes the correct path.
6. After every meaningful experiment, update the plan with:
   - exact configuration;
   - corpus subset;
   - measurements;
   - visual observations;
   - performance impact;
   - whether the hypothesis was supported, rejected, or inconclusive;
   - the resulting decision;
   - the next action.
7. Preserve failed experiments and rejected hypotheses in the plan.
8. Keep every meaningful quality parameter runtime-configurable so parameter sweeps do not require recompilation or source edits.
9. Preserve the current output path as a selectable control.
10. Separate neural reconstruction quality from postpass composition and Qt presentation quality.
11. Do not alter model weights, network topology, the native INT8 graph, codec-motion handling, or add expensive optical flow unless the controlled `learned_only` experiment proves the defect exists before the postpass/presentation stage.
12. Run relevant tests and benchmark subsets after behavior changes.
13. Record GPU and pipeline timing for finalist configurations.
14. Prefer controlled evidence over intuition.
15. Do not declare completion merely because implementation tasks are checked off.

## Completion condition

Continue until the plan's defined completion criteria are satisfied or you encounter a genuine blocker that cannot be resolved from repository evidence, available tools, or controlled experimentation.

A successful campaign must identify the source of the staircase artifact, materially improve smoothness and detail, address the tone/exposure discrepancy, verify temporal stability, preserve acceptable performance, and leave the winning configuration reproducible from runtime configuration.

Do not stop at "the code works."

Stop when the evidence supports a materially better Temporal Forge reconstruction path.
