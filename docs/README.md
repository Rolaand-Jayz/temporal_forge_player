# Temporal Forge documentation

This is the documentation entry point. The governing structure is defined by
[`DOCUMENTATION_SYSTEM.md`](DOCUMENTATION_SYSTEM.md).

| Need | Start here |
|---|---|
| What is true now? | [`current/STATE.md`](current/STATE.md) |
| How does the current system work? | [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md), [`reference/environment.md`](reference/environment.md) (the `TFORGE_*` environment contract) |
| What is being worked on now? | [`active/QUALITY_CAMPAIGN.md`](active/QUALITY_CAMPAIGN.md) (the single active quality authority) and [`active/PORTABILITY_REMEDIATION_20260909.md`](active/PORTABILITY_REMEDIATION_20260909.md) |
| Where is the campaign progress log and historical gate record? | [`active/progress-state.md`](active/progress-state.md) (supporting record, not an authority) |
| Why did the design change? | [`decisions/TECHNICAL_HISTORY.md`](decisions/TECHNICAL_HISTORY.md) |
| What did a dated campaign find? | [`reports/`](reports/) (e.g. the review adjudication of 2026-09-06, [`reports/QUALITY_LAB_REVIEW_ADJUDICATION_20260906.md`](reports/QUALITY_LAB_REVIEW_ADJUDICATION_20260906.md)) and the benchmark READMEs |
| Where is exploratory research? | [`research/`](research/) |
| Where are completed plans and old prompts? | [`archive/`](archive/) |
| Where is detailed measurement evidence? | [`../benchmarks/quality_sweeps/`](../benchmarks/quality_sweeps/) |

Root-level [`README.md`](../README.md) describes the product. Root-level
[`AGENTS.md`](../AGENTS.md) contains operating instructions and points back to
this map.

## Authority map

- Current state: [`current/STATE.md`](current/STATE.md).
- Current architecture and durable implementation invariants:
  [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md).
- Current FSR input and motion contracts: [`reference/motion/`](reference/motion/).
- Active quality work: [`active/QUALITY_CAMPAIGN.md`](active/QUALITY_CAMPAIGN.md)
  is the single canonical active quality authority.
- Active portability/reproducibility remediation:
  [`active/PORTABILITY_REMEDIATION_20260909.md`](active/PORTABILITY_REMEDIATION_20260909.md).
- [`active/progress-state.md`](active/progress-state.md) is a supporting
  orchestration/progress log for the quality workstream. It records what was
  done and when; it does not direct current execution. Where it and the
  active plan disagree, the active plan governs.
- Detailed measurements: benchmark manifests and artifacts, not copied tables
  in narrative documents.
- Completed plans, dated reports, and research are historical or exploratory
  unless they explicitly link a verified conclusion into a current document.
