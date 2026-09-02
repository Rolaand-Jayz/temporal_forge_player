# Temporal Forge Documentation System

**Status:** CURRENT
**Purpose:** Define how Temporal Forge documentation is organized, interpreted, maintained, archived, and given authority.

---

# 1. Objective

Temporal Forge documentation must make it easy to distinguish:

```text
WHAT IS TRUE NOW
        ↓
HOW THE CURRENT SYSTEM WORKS
        ↓
WHAT IS BEING WORKED ON NOW
        ↓
WHY THAT WORK IS HAPPENING
        ↓
WHAT HAPPENED BEFORE
        ↓
WHAT WAS LEARNED
        ↓
WHERE THE PRIMARY EVIDENCE LIVES
```

The documentation system must preserve project history without allowing historical documents to masquerade as current instructions.

The central information problem is temporal:

> Temporal Forge has changed quickly, and many documents accurately describe different moments in the project's development.

Those documents are valuable.

The failure occurs when their time, purpose, status, or authority is ambiguous.

---

# 2. Governing Reasoning Rule

## Logic outranks convention

Documentation practices, methodologies, naming schemes, ADR conventions, folder structures, and other industry patterns are useful heuristics.

They are not automatically correct.

When convention conflicts with a structure that more accurately represents Temporal Forge:

> Choose the logically superior structure.

Prioritize:

1. logical consistency;
2. unambiguous authority;
3. causal traceability;
4. discoverability;
5. preservation of evidence;
6. maintainability;
7. convention.

Do not create organizational machinery simply because it is considered a “best practice.”

Do create unconventional organization when it solves the information problem more directly.

---

# 3. Documentation Classes

Every meaningful documentation artifact should have one primary semantic role.

The exact directory names may evolve, but these concepts must remain distinguishable.

## 3.1 Current State

Answers:

> What is Temporal Forge right now?

Contains concise current information such as:

- current phase;
- current implementation state;
- current baseline;
- current known limitations;
- what is verified;
- what is experimental;
- current major open questions;
- links to deeper current references.

This is living documentation.

It should not accumulate a full development diary.

---

## 3.2 Current Reference / Architecture

Answers:

> How does the current system actually work?

Includes living technical reference such as:

- architecture;
- pipeline semantics;
- current data contracts;
- current FSR integration behavior;
- current experiment/provenance architecture;
- durable implementation invariants.

A document claiming to be authoritative current reference must match current code or explicitly identify known divergence.

---

## 3.3 Active Work

Answers:

> What should be worked on now?

Contains:

- current goal;
- immediate context;
- active hypotheses;
- execution order;
- gates;
- blockers;
- Definition of Done;
- links to motivating evidence.

There should normally be one obvious active plan for a particular active workstream.

An active plan must not become the permanent repository history.

When completed or superseded, archive/reclassify it.

---

## 3.4 Decisions / Causal History

Answers:

> Why did the project change direction?

Preserve important chains such as:

```text
problem
→ hypothesis
→ experiment
→ evidence
→ conclusion
→ decision
→ implementation consequence
```

Not every code change needs a decision record.

Preserve decisions that materially explain current architecture or future reasoning.

A decision may be recorded individually or in a coherent decision/history document depending on which is logically clearer.

---

## 3.5 Reports / Experimental Results

Answers:

> What happened in a specific experiment or campaign?

These are usually dated.

Examples:

- supersampling report;
- recapture report;
- acceptance report;
- benchmark analysis;
- quality experiment conclusion.

Reports describe evidence observed under a particular project state.

They do **not** automatically define current architecture.

Current documents may cite them as evidence.

---

## 3.6 Research

Answers:

> What external or exploratory information informed our thinking?

Research may contain:

- external technical investigation;
- reverse-engineering source analysis;
- speculative possibilities;
- candidate mechanisms.

Research does not silently become verified Temporal Forge behavior.

Accepted conclusions must move into appropriate current reference or decision documentation.

---

## 3.7 Archive

Contains material whose original role is no longer active, including:

- completed plans;
- superseded plans;
- old gates;
- obsolete instructions;
- historical goal prompts;
- old architecture snapshots where preservation is useful.

Archived material remains evidence of what was known or intended at that point in time.

Archive status must make it difficult to mistake the material for current instruction.

---

# 4. Current Truth vs Historical Truth

A statement may have been correct at one time and wrong today.

Both may be worth preserving.

Example:

```text
2026-08-31:
3× supersampling appeared promising under tested conditions.

Later evidence:
2160p delivery showed direct reconstruction temporally outperforming
the oversampling arms.

Current conclusion:
3× supersampling is resolution-dependent and not a universal default.
```

Do not erase the earlier observation.

Do not present both as simultaneously current.

---

# 5. Historical Integrity

Never rewrite historical documents to create fake hindsight.

If an experiment originally proceeded from an incorrect hypothesis, preserve it.

Correct representation:

```text
At the time:
X was believed because evidence A supported it.

Experiment:
B tested X.

Result:
B contradicted X.

Decision:
X was replaced by Y.
```

Incorrect representation:

```text
Rewrite the original plan so it claims Y was known all along.
```

The evolution of understanding is technically valuable.

---

# 6. Evidence and Interpretation

Documentation must distinguish where materially important:

- verified fact;
- measured result;
- observation;
- inference;
- hypothesis;
- decision;
- current implementation state;
- intended behavior;
- known defect.

If current code and intended architecture differ, document both.

Example:

```text
INTENDED:
FSR-style jitter is applied during the prepass input resolve.

ACTUAL:
The current baseline still requires an experimental profile to activate it.

STATUS:
Known implementation/baseline divergence.
```

Never conceal a divergence merely to make documentation cleaner.

---

# 7. Authority

One concept should have one current authoritative home.

Avoid several living documents independently defining:

- current architecture;
- current FSR input semantics;
- current baseline;
- active campaign;
- current backend behavior;
- current experiment system.

Other documents should link to the authoritative source rather than maintaining competing descriptions.

Historical documents may contain older descriptions as long as historical status is clear.

---

# 8. Authority Claims Must Be Verified

Search carefully for language such as:

- source of truth;
- authoritative;
- current;
- active;
- baseline;
- default;
- final;
- approved;
- recommended.

A document making such claims must actually hold that role.

If not:

- update it;
- demote it;
- archive it;
- or remove the authority claim.

Do not tolerate authority drift.

---

# 9. Documentation Entry Point

`docs/` must have one obvious starting point.

Recommended:

`docs/README.md`

It should function as a map, not a technical encyclopedia.

It should answer:

| Need | Go here |
|---|---|
| What is Temporal Forge today? | Current State |
| How does it work? | Current Architecture / References |
| What are we doing now? | Active Work |
| Why did architecture X change? | Decisions / History |
| What did campaign Y find? | Reports |
| Where are old plans? | Archive |
| Where is exploratory research? | Research |
| Where is raw benchmark evidence? | Benchmark/evidence system |

A new agent should not have to guess which document to read first.

---

# 10. Current-State Document

Maintain one concise current-state document.

It should normally contain:

- as-of date;
- relevant Git/source identity where useful;
- project purpose;
- current development phase;
- current FSR implementation status;
- current baseline identity;
- major current architecture;
- verified findings;
- major uncertainties;
- current known defects;
- current active work;
- next major hypothesis;
- performance state;
- major project boundaries;
- links to deeper documentation.

It should summarize **now**.

Do not append the complete historical execution diary.

---

# 11. Current Architecture

Current architecture documentation must describe current code.

If `docs/reference/ARCHITECTURE.md` remains the authoritative document, maintain it accordingly.

If a more logical set of current-reference documents replaces it, clearly identify the replacement.

Architecture documentation should emphasize durable structure and invariants rather than dated experiment results.

Experiment results belong in reports/history and may be cited as the reason an architectural decision exists.

---

# 12. Active Plans

Active plans are disposable in authority, not disposable in history.

Lifecycle:

```text
PROPOSED
   ↓
ACTIVE
   ↓
COMPLETED
or
SUPERSEDED
or
ABANDONED
   ↓
ARCHIVE / HISTORY
```

When an active plan completes:

1. persist its meaningful results;
2. update current-state/reference documentation where behavior changed;
3. record major decisions;
4. classify/archive the plan;
5. create a new active plan if more work follows.

Do not leave several completed plans appearing active.

---

# 13. Active Plans Must Not Become Lab-Notebook Monoliths

Avoid repeating the current `QUALITY_RECONSTRUCTION_PLAN.md` failure mode.

An active plan should not indefinitely become:

```text
plan
+
daily journal
+
all experiment results
+
architecture spec
+
decision log
+
historical timeline
+
status dashboard
```

While an active plan may be updated with enough result information to drive its next decision gate, mature results should move to their proper semantic homes.

The plan should remain understandable as **the plan**.

---

# 14. Completed Plans

Completed or superseded plans remain useful because they answer:

> What did we intend to do given what we knew at that time?

Preserve them.

Prefer `git mv` when reorganizing so history remains easy to trace.

Possible status information:

```text
Status: COMPLETED
Completed: YYYY-MM-DD
Result: ...
Current successor: ...
```

or maintain equivalent information in an archive index if modifying every historical document would create unnecessary churn.

---

# 15. Reports

Dated reports represent observations from a specific period.

Examples currently include:

- `FSR4_SUPERSAMPLING_REPORT_20260831.md`
- `M6_RECAPTURE_REPORT_20260901.md`
- M6 acceptance/baseline reports;
- scope audits.

These should remain easy to discover as historical evidence.

They should not appear to be current execution instructions.

---

# 16. Research Material

Research filenames must communicate their subject sufficiently to be discoverable.

Avoid meaningless names such as:

`deep-research-report(14).md`

when a semantic name can be determined.

When renaming:

- preserve original attribution;
- preserve content;
- use Git history;
- do not imply the research has been accepted as fact.

---

# 17. Causal Technical History

Maintain a project-level technical history sufficient to explain major transitions without reading every commit.

It should focus on meaningful phases rather than every code modification.

A useful entry structure:

```text
## Phase / Date Range

Problem:
What needed to be solved?

Working belief:
What did we think was happening?

Investigation:
What was tested or changed?

Evidence:
What happened?

Conclusion:
What did the evidence support or falsify?

Decision:
What changed because of this?

Superseded:
What earlier belief/approach stopped being current?

Remaining:
What stayed unresolved?

Evidence:
Links to reports, commits, manifests, or archived plans.
```

Chronology establishes order.

Causality explains why the order matters.

---

# 18. Git History

Git history is evidence, not the user interface for understanding the project.

Use Git to reconstruct:

- dates;
- implementation transitions;
- document evolution;
- commit ancestry.

Then represent meaningful project history explicitly.

A human should not need to run `git log` merely to answer:

> Why does the pipeline work this way?

---

# 19. Contradiction Resolution

When documentation conflicts, determine truth logically using evidence.

A useful starting hierarchy:

```text
validated actual runtime behavior
        ↓
current code
        ↓
validated current experiment provenance
        ↓
current Git state
        ↓
dated experiment evidence
        ↓
current decision/reference documents
        ↓
historical plans
        ↓
research hypotheses
        ↓
unverified narrative/comments
```

This is not an inflexible rule.

For example, current code can contain a known regression.

In that case:

```text
actual = current code
intended = documented architecture
status = known defect
```

Document the disagreement rather than declaring one invisible.

---

# 20. Benchmark Evidence

Raw benchmark/evidence systems remain authoritative for detailed measurements.

Documentation should generally provide:

```text
conclusion
+
critical supporting numbers
+
link/path to primary evidence
```

Avoid copying giant result tables into several Markdown files.

Duplication produces contradiction.

---

# 21. Documentation Maintenance Lifecycle

For meaningful work:

```text
question
    ↓
active plan
    ↓
implementation / experiment
    ↓
evidence
    ↓
conclusion
    ↓
decision
    ↓
update current state/reference
    ↓
archive completed plan/report as appropriate
```

A change is not fully integrated merely because code was merged.

If it changes what Temporal Forge **is**, update current documentation.

If it changes why Temporal Forge works a particular way, preserve the decision/history.

---

# 22. AGENTS.md Contract

`AGENTS.md` must contain a concise documentation-maintenance section.

Future agents must be instructed to:

1. start with `docs/README.md` or the current documentation entry point;
2. treat `docs/DOCUMENTATION_SYSTEM.md` as the authority for documentation organization;
3. read the current-state document before relying on historical reports;
4. use the designated active-work document for current execution;
5. keep current architecture/reference synchronized with actual code;
6. move completed/superseded plans out of the active namespace;
7. never turn the active plan into an indefinite project-history journal;
8. keep reports historical;
9. keep research distinct from verified facts;
10. update current reference/state after accepted implementation changes;
11. record meaningful causal decisions;
12. resolve documentation authority conflicts rather than adding another competing source.

`AGENTS.md` should link to this document rather than reproduce it.

---

# 23. This Document's Own Lifecycle

`docs/DOCUMENTATION_SYSTEM.md` is a **living current-reference document**.

It defines the current documentation model.

Therefore:

- keep it concise enough to remain usable;
- update it when the documentation model materially changes;
- do not append routine cleanup history to it;
- do not turn it into a migration log;
- preserve major historical changes to the documentation model in project history/decisions where useful.

This document is subject to its own rules.

---

# 24. Repository Areas Requiring Initial Classification

The initial cleanup must explicitly inspect:

## Root and entry points

- `README.md`
- `AGENTS.md`
- `CONTRIBUTING.md`
- `docs/README.md`
- `docs/DOCUMENTATION_SYSTEM.md`

## `docs/`

- `current/STATE.md`
- `reference/ARCHITECTURE.md` and current contracts;
- `active/QUALITY_CAMPAIGN.md` and active status;
- `decisions/TECHNICAL_HISTORY.md`
- `reports/` and `reports/audits/`
- `research/`
- `archive/`
- other documentation discovered during inventory.

## Archived plans

At minimum:

- `docs/archive/plans/QUALITY_PERFECTION_EXECUTION_SLICES.md`
- M0–M5 gate documents;
- M6 regression/tooling documents;
- `docs/archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md`
- the original master-plan source if present.

## Elsewhere

Inspect benchmark/campaign READMEs and other files that materially explain experiment interpretation.

---

# 25. Special Case — `QUALITY_RECONSTRUCTION_PLAN.md`

This document is historically valuable but semantically overloaded.

It currently includes material from several periods and roles.

Its content must be classified into logical destinations such as:

- historical plan;
- historical execution/results;
- major conclusions;
- current architecture decisions;
- current active work, if any remains;
- causal project history.

Do not simply delete it.

Do not leave it as the singular current authority merely because it accumulated the most information.

The goal is to preserve its knowledge while removing its authority ambiguity.

---

# 26. Initial Project Timeline

During the cleanup, reconstruct major Temporal Forge phases from actual evidence.

Likely major areas include:

- initial player architecture;
- FSR4 reverse-engineering reconstruction;
- graph/structural proof;
- quality-perfection work;
- stable-base/postpass investigation;
- temporal investigations;
- motion-vector work;
- supersampling/downsampling;
- FSR input-semantics discoveries;
- quality-capture/provenance incident;
- recapture/recovery;
- AMD-semantic baseline recovery;
- correspondence/optical-flow/natural-temporal-sampling hypothesis.

This list is only a search aid.

Do not force it onto repository history if evidence shows a different sequence.

---

# 27. Validation Questions

After reorganization, a new developer or agent should be able to answer with minimal ambiguity:

1. What is Temporal Forge today?
2. What is its current FSR4 architecture?
3. What are the intended current video→FSR input semantics?
4. What behavior is verified?
5. What behavior remains experimental?
6. What is the current active task?
7. Why is that task being done?
8. What major work is already complete?
9. Why did the jitter path change?
10. Why are codec motion vectors being reconsidered?
11. What did supersampling establish?
12. Why isn't 3× simply the universal default?
13. What happened during the capture/provenance failure?
14. What evidence had to be recaptured?
15. Which historical plans should not be executed now?
16. Where is the evidence supporting a particular old conclusion?
17. What should happen next?

If the answer requires guessing which apparently authoritative document is newer, the organization is still defective.

---

# 28. Concurrency Safety During Initial Cleanup

The initial reorganization may happen while quality capture automation is running.

During that operation:

- do not stop or pause capture;
- do not modify capture/benchmark scripts;
- do not alter active experiment manifests;
- do not move generated evidence;
- do not rewrite review-harness outputs;
- do not rebuild/replace the binary;
- do not change source behavior;
- do not switch branches;
- do not clean/reset the working tree;
- do not stage unrelated capture-generated files.

Documentation changes should remain isolated.

---

# 29. Definition of Done — Initial Reorganization

The initial documentation cleanup is complete only when all of the following are true.

## Entry and discoverability

- [x] There is one obvious documentation entry point.
- [x] A newcomer knows where to find current state.
- [x] A newcomer knows where to find architecture/reference.
- [x] A newcomer knows where current active work lives.
- [x] A newcomer knows where historical reports/plans live.
- [x] A newcomer knows where research lives.
- [x] A newcomer knows where primary experiment evidence lives.

## Current authority

- [x] One clear source describes current project state.
- [x] Current architecture has an explicit authoritative home.
- [x] Current FSR/input semantics have an explicit authoritative home.
- [x] Current active work has an explicit authoritative home.
- [x] Current claims have been checked against repository/code state.
- [x] Stale documents no longer claim current authority.

## Historical organization

- [x] Completed M0–M6 work is correctly classified.
- [x] Completed/superseded plans are separated from active plans.
- [x] Dated reports are clearly historical.
- [x] Historical hypotheses remain historically accurate.
- [x] Major changes can be followed chronologically.
- [x] Major changes explain causal reasoning.

## Authority conflicts

- [x] `README.md`, `ARCHITECTURE.md`, `AGENTS.md`, current-state documentation, and active-plan documentation do not make incompatible authority claims.
- [x] `QUALITY_RECONSTRUCTION_PLAN.md` no longer has ambiguous all-purpose authority.
- [x] Research documents cannot reasonably be mistaken for current implementation specifications.

## Names

- [x] Ambiguous names are corrected where the benefit outweighs churn.
- [x] Current living references and dated historical reports are visually/semantically distinguishable.
- [x] Original historical meaning remains traceable through Git.

## Maintenance

- [x] `AGENTS.md` contains the concise documentation-maintenance contract.
- [x] `AGENTS.md` points to this document.
- [x] Future agents are instructed to archive completed plans.
- [x] Future agents are instructed to update current-state/reference documentation after accepted changes.
- [x] Future agents are instructed not to use active plans as indefinite project journals.
- [x] Future agents are instructed to preserve causal history.

## Safety

- [x] Running capture automation was not interrupted.
- [x] Capture evidence was not modified or deleted.
- [x] No unrelated source behavior changed.
- [x] No binary was rebuilt/replaced by this task.
- [x] Documentation changes remained isolated from automation-owned changes.

## Final usability

The documentation supports the path:

```text
CURRENT STATE
    ↓
CURRENT ARCHITECTURE
    ↓
ACTIVE WORK
    ↓
RATIONALE / DECISIONS
    ↓
PROJECT HISTORY
    ↓
PRIMARY EVIDENCE
```

without requiring repository archaeology for ordinary understanding.

---

# 30. Governing Principle

The documentation system exists to preserve **meaning across time**.

Temporal Forge can change rapidly.

The documentation must allow a reader to determine:

> what is true now, what used to be true, what was only believed, what was tested, what was learned, why the direction changed, and what should happen next.

If those distinctions remain obvious, the precise folder layout is secondary.

If those distinctions are ambiguous, a conventionally “clean” folder layout is still a failure.
