# Documentation reorganization audit

**Date:** 2026-09-02
**Branch:** `quality-lab-vibecoder`
**Scope:** documentation only; the concurrent capture process and its generated
artifacts were not stopped, paused, signaled, edited, staged, or deleted.

## Evidence-backed authority correction

The governing specification names `docs/DOCUMENTATION_SYSTEM.md`, but that
path did not exist. The supplied specification was present as the untracked
`docs/Temporal Forge Documentation System.md`. Its content was preserved and
renamed to the specified canonical path. This is the smallest correction that
restores the authority path required by the specification. No rule in the
specification was independently redesigned.

## Classification

| Area/document | Actual role | Time/status | Authority treatment |
|---|---|---|---|
| `docs/DOCUMENTATION_SYSTEM.md` | documentation system reference | current/living | governing documentation model |
| `docs/current/STATE.md` | current state | current | sole concise current-state summary |
| `docs/reference/ARCHITECTURE.md` | architecture and durable invariants | current | current implementation reference; checked against `src/` |
| `docs/reference/VIBECODER_DEVELOPER_GUIDE.md` | developer reference | current reference | supporting reference, not state authority |
| `docs/reference/motion/` | current motion and jitter contracts | current reference | contract authority for those interfaces |
| `docs/reference/testing/` | test strategy | current reference | test/evidence guidance |
| `docs/active/QUALITY_CAMPAIGN.md` | focused quality campaign plan | active | current active-work authority |
| `docs/active/progress-state.md` | campaign status and links | active status record | supporting active status, not architecture authority |
| `docs/active/M6_REGRESSION_TRIAGE.md` | unresolved M6 issue | active | issue record linked from active work |
| `docs/decisions/TECHNICAL_HISTORY.md` | causal project history | current index of history | explains transitions, links to evidence |
| `docs/reports/` | dated reports and experiment observations | historical | evidence summaries, not current instructions |
| `docs/reports/audits/` | dated audit reports | historical | audit evidence, not current authority |
| `docs/research/` | exploratory external/technical research | exploratory | cannot define current behavior silently |
| `docs/archive/plans/` | completed/superseded plans and gates | historical | preserved, removed from active namespace |
| `docs/archive/prompts/` | historical launch prompt | historical | preserved, not an instruction source |
| root `README.md` | product/user overview | current summary | links to the documentation map; no competing architecture authority |
| root `AGENTS.md` | agent operating instructions | current instructions | concise documentation contract; quality rules remain separate |
| root `CONTRIBUTING.md` | contributor workflow | current contributor guide | points to current reference and entry point |
| `benchmarks/quality_sweeps/` and `benchmarks/video_corpus/` READMEs | evidence/tool usage | current tool references plus dated results | raw manifests and artifacts remain primary measurement evidence |
| `benchmarks/quality_sweeps/swarm/` READMEs | experiment notes | historical/agent-specific | retained in place; not treated as current authority |
| `resources/fsr4/native_i8/**/README.md` | resource/build artifact notes | resource-local | authoritative only for its resource, not project architecture |
| `review_harness/README.md` | harness usage | current tool reference | harness semantics only |

## Findings and repairs

1. The old reconstruction plan was simultaneously active plan, architecture
   reference, experiment notebook, result database, and history. It is now
   preserved at `docs/archive/plans/QUALITY_RECONSTRUCTION_PLAN_20260822-20260902.md`
   with an explicit historical status, while the focused active plan lives at
   `docs/active/QUALITY_CAMPAIGN.md`.
2. Completed M0-M5 and M6 tooling gates, the completed slice plan, and the old
   refactor plan moved to the archive. Their content and Git ancestry remain
   available.
3. Dated FSR, M6, and scope-audit documents moved to reports. The meaningless
   `deep-research-report(14).md` name became
   `research/fsr4-video-input-and-motion-research.md`; its exploratory status
   is explicit.
4. The current FSR4 default-selection wording was corrected in the product and
   architecture summaries to match `BackendSelector::select`: proof-gated FSR4
   INT8 is tried first on supported RDNA3, then fallback paths are selected.
5. Old moved-path references were repaired where they represented current
   navigation. References inside preserved historical documents remain
   historical context unless they blocked navigation from a current document.

## Causal history reconstructed

The project history now has an explicit index covering the initial player
architecture, FSR4 reconstruction/proof gating, temporal input contracts,
supersampling findings, and the capture/provenance recovery. The index preserves
uncertainty and points to dated reports and benchmark evidence rather than
rewriting old conclusions as present knowledge.

## Definition of Done

### Entry and authority

- [x] One entry point exists at `docs/README.md`.
- [x] Current state, current architecture, active work, history, reports,
  research, archive, and primary evidence are each discoverable.
- [x] Current state is `docs/current/STATE.md`.
- [x] Current architecture is `docs/reference/ARCHITECTURE.md`.
- [x] Current FSR input/motion contracts are in `docs/reference/motion/`.
- [x] Active campaign authority is `docs/active/QUALITY_CAMPAIGN.md`.
- [x] Raw benchmark artifacts remain the detailed measurement source.

### History and names

- [x] Completed and superseded plans left the active namespace.
- [x] Dated reports and research are semantically separated from current docs.
- [x] The overloaded reconstruction plan is preserved without active authority.
- [x] The research filename is meaningful and original content is preserved.
- [x] Causal transitions are indexed without inventing missing evidence.

### Maintenance and safety

- [x] `AGENTS.md` points to the system and entry point with a concise contract.
- [x] Current summaries were checked against repository code where claims changed.
- [x] The capture process was not interrupted and capture-generated data was not
  staged or modified.
- [x] No source, capture script, manifest, binary, harness asset, or generated
  experiment evidence was changed by this work.

## Remaining limitation

The concurrent capture may change the live evidence directory after this audit.
That directory is intentionally outside this documentation commit. A later
campaign completion update must refresh `current/STATE.md`, the active plan,
and dated reports from its final manifests rather than treating this audit as
capture completion.
