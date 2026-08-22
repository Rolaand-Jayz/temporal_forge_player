# Temporal Forge Agent Instructions

Temporal Forge is currently in a **QUALITY-FOCUSED phase**.

## Primary priority

Improve reconstruction and presentation quality while preserving the existing fast native Vulkan/RDNA3 path.

## Scope discipline

Do not perform any of the following unless explicitly required by evidence from the active quality plan:

- UI redesign
- new player features
- unrelated refactoring
- model retraining
- weight changes
- convolution/topology changes
- expensive optical flow
- broad FSR reverse-engineering work
- architecture churn unrelated to image quality

The player already functions well enough for this phase. UI and product polish are deferred.

## Active execution plan

Read and follow:

`docs/exec-plans/QUALITY_RECONSTRUCTION_PLAN.md`

The execution plan is authoritative for the current quality campaign.

## Working rules

1. Read the complete active plan before changing code.
2. Treat the experiment ordering, decision gates, and completion criteria as authoritative.
3. Keep experimental quality parameters runtime-configurable. Do not require source edits or recompilation to test another strength/filter/value.
4. Preserve the existing baseline path as a selectable control.
5. Update the execution plan as work proceeds.
6. Record measurements, observations, rejected hypotheses, and conclusions directly in the plan.
7. Do not mark an experiment complete without its required validation.
8. Do not continue blindly into later stages when an earlier result changes the correct path.
9. Preserve reproducible benchmark artifacts and configuration files.
10. Run the relevant tests and benchmark subset after behavior changes.
11. Separate reconstruction defects from Qt/player presentation defects.
12. Prefer the smallest change that directly tests the active hypothesis.
13. Quality is the sole development priority for this phase. Avoid feature creep.

## Evidence standard

The goal is not to make the picture subjectively prettier by stacking arbitrary filters.

The goal is to identify where remaining visual errors originate and correct the responsible stage.

Every retained quality change must be:

- reproducible from configuration;
- supported by captured output;
- measured against appropriate references;
- checked for performance impact;
- checked for new artifacts;
- documented with the reason it was retained.

Failed experiments are valid results. Record them and move on.

## Completion behavior

Continue working through the active plan until its completion criteria are satisfied or a genuine blocker is reached that cannot be resolved from repository evidence, available tooling, or controlled experimentation.

Do not declare completion merely because the planned infrastructure or experiment code exists.
