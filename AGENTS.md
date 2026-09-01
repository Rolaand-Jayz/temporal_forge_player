# Temporal Forge Agent Instructions

Temporal Forge is currently in a **QUALITY-FOCUSED phase**.

## Orchestration model

GLM 5.3 is the orchestrator. GLM 5.3 Flash workers execute.

1. GLM 5.3's primary role is orchestration: decomposition, architecture,
   planning, cross-cutting decisions, conflict resolution, escalation, and
   final synthesis. Keep routine execution off the orchestrator.
2. GLM 5.3's secondary role is high-difficulty implementation. GLM 5.3
   implements directly only when the task is demonstrably beyond GLM 5.3
   Flash capability — deep architectural reasoning, ambiguous cross-cutting
   changes, or resolving conflicting worker results. Everything else is
   delegated.
3. All implementation, testing, routine debugging, repository exploration,
   mechanical refactoring, benchmark runs, and repetitive execution go to
   GLM 5.3 Flash workers by default.
4. Flash workers have outstanding long-running task capabilities. Reflect
   this in orchestration:
   - Give workers complete, well-scoped tasks with explicit ownership,
     constraints, expected outputs, and verification requirements — not
     fragmented micro-steps.
   - Let long jobs run. Do not busy-poll or interrupt workers mid-task;
     use long waits and check only when the result is needed for the next
     critical-path decision.
   - Run independent work streams in parallel with disjoint write sets.
5. The orchestrator works on the critical-path item locally while workers
   run in parallel. Do not delegate-and-wait.
6. GLM 5.3 is blind. GLM 5.3 Flash can see. Flash workers act as the eyes
   of the orchestrator:
   - All visual inspection — captured frames, screenshots, rendered output,
     image comparisons, UI, charts, video frames, layout, pixels — is
     delegated to Flash.
   - The orchestrator never invents or assumes visual details. Visual
     claims come only from concrete Flash observations with actionable
     findings.
   - For visually judged work, require Flash workers to visually verify
     their own output and report what they actually saw.

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

## Personal-device performance safeguard

During the **7:45 PM–9:00 PM Central-time window** (CDT or CST), check the
running process list for Trackmania (case-insensitively, including
launcher/game variants). This time-window check is independent of the type of
work being done. Quality-only work and quality captures may continue with
Trackmania running; gaming can affect performance results, but must not be
treated as affecting image-quality results.

Performance work is a separate pause condition at any time: before starting
or continuing performance work, check whether Trackmania is running and, if
it is, halt the performance work until the game is no longer running. Never
terminate the game or other user processes without explicit permission.
For all sample-producing runs, monitor the process list for games and common
game launchers, not only Trackmania. Do not parallelize sample collection when
any detected game is running. The capture harness must pause its own child
process group while a detected game is running and resume only after it clears.
An explicit user instruction may allow a named game pattern for that run; pass
it as `--allow-game PATTERN` and record the exception. Never terminate the game
or other user processes. Record detections, pauses, resumes, and any explicit
allow-list exception in the work update.
This is a repository operating rule; it does not create an autonomous timer
or wake the agent when no session is active.

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
