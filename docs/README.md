# Temporal Forge documentation

This is the documentation entry point. The governing structure is defined by
[`DOCUMENTATION_SYSTEM.md`](DOCUMENTATION_SYSTEM.md).

| Need | Start here |
|---|---|
| What is true now? | [`current/STATE.md`](current/STATE.md) |
| How does the current system work? | [`../ARCHITECTURE.md`](../ARCHITECTURE.md) |
| What is being worked on now? | [`active/QUALITY_CAMPAIGN.md`](active/QUALITY_CAMPAIGN.md) (the single active quality authority) |
| Where is the campaign progress log and historical gate record? | [`active/progress-state.md`](active/progress-state.md) (supporting record, not an authority) |
| Where is the historical FSR4 reverse-engineering research log? | [`FSR4_RE_STATUS.md`](FSR4_RE_STATUS.md) (dated historical research) |
| Why did the design change? | [`reports/QUALITY_LAB_REVIEW_ADJUDICATION_20260906.md`](reports/QUALITY_LAB_REVIEW_ADJUDICATION_20260906.md) |
| What did a dated campaign find? | [`reports/`](reports/) and the benchmark READMEs |
| Where is exploratory research? | benchmark analysis under [`../benchmarks/quality_sweeps/`](../benchmarks/quality_sweeps/) |
| Where are completed plans and old prompts? | [`archive/`](archive/) |
| Where is detailed measurement evidence? | [`../benchmarks/quality_sweeps/`](../benchmarks/quality_sweeps/) |

Root-level [`README.md`](../README.md) describes the product.

## Authority map

- Current state: [`current/STATE.md`](current/STATE.md).
- Current architecture and durable implementation invariants:
  [`../ARCHITECTURE.md`](../ARCHITECTURE.md).
- Current FSR input and motion contracts:
  [`../ARCHITECTURE.md`](../ARCHITECTURE.md) and
  [`../tests/motion_estimator_tests.cpp`](../tests/motion_estimator_tests.cpp).
- Active quality work: [`active/QUALITY_CAMPAIGN.md`](active/QUALITY_CAMPAIGN.md)
  is the single canonical active quality authority.
- [`active/progress-state.md`](active/progress-state.md) is a supporting
  orchestration/progress log for the same workstream. It records what was done
  and when; it does not direct current execution. Where it and the active plan
  disagree, the active plan governs.
- Detailed measurements: benchmark manifests and artifacts, not copied tables
  in narrative documents.
- Completed plans, dated reports, and research are historical or exploratory
  unless they explicitly link a verified conclusion into a current document.
