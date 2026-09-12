# Temporal Forge Worktree Cleanup Report

**Date:** 2026-09-11  
**Purpose:** Prepare the repository for the FSR Expected-Food campaign  
**Canonical repository:** `/mnt/workdrive/ZCodeProject/temporal_forge_player`  
**Status:** COMPLETE; READY TO BRANCH THE EXPECTED-FOOD CAMPAIGN

## Executive result

The repository was reduced from 15 registered worktrees to two intentional
worktrees. The canonical checkout is clean at
`12bc740e481741bf1d7e6c439fe2b73dc41bc55d` on `quality-lab-vibecoder`.
Historical branches were retained. Local evidence was either retained in place
or archived and verified before its worktree was removed. The Expected-Food
campaign itself was not started.

## Instruction-file change

The authoritative repository instruction file is:

`/mnt/workdrive/ZCodeProject/temporal_forge_player/AGENTS.md`

The original quality-phase instructions were preserved. A repository-scope
section named **Mandatory worker completion wait protocol** was appended. It
requires non-polling worker completion waits, 10,000-second blocking holds
when dependent work is blocked, worker interruption of the hold on completion,
and repeated holds until all required workers terminate.

Verification performed:

- the original instruction content remains present;
- the appended protocol is present at repository scope;
- no more-specific `AGENTS.md` exists below the repository root;
- the active plan `docs/active/QUALITY_CAMPAIGN.md` remains present.

## Initial inventory and disposition

| Path | Initial HEAD | Initial state | Final disposition |
|---|---|---|---|
| `temporal_forge_player` | `738ca55c6d2843c342cd8b4e66131ce5f8dddf56` | 24 tracked changes; 1,908 untracked paths | Canonical; retained and checkpointed |
| `review-baseline` | `6ae08932f2a5a8cd870240a83782a5799acfabca` | clean tracked state; 144 untracked files under `.pr1_validation` and `.pr2_validation` | Archived, verified, removed |
| `review-candidate-a` | `db19cfb34f830d4dce147c4d662b5c4b9557d6a4` | clean, detached; duplicate historical candidate | Removed directly |
| `review-candidate-b` | `2d76a2e720f7024cad2c04aaf5fe3e4ed880abf7` | clean tracked state; 198 untracked files under `.review` and `external` | Archived, verified, removed |
| `temporal-forge-confidence-fallback` | `b1ff7b9c015ab08981db774d1add58ea015069b7` | clean | Removed directly |
| `temporal-forge-devil` | `83a687aa010791aaeeca2bcbb9feda758c14a155` | clean tracked state; 4,806 untracked files; approximately 820 MiB campaign evidence | Historical motion campaign; retained |
| `tf-adjudication` | `db0af42d993140578456a26bbf08c7c6107a5238` | clean | Removed directly |
| `tf-adjudication-fallback` | `7160a7edd7b6d944cd3c242287d41137823f5d58` | clean | Removed directly |
| `tf-portability-remediation` | `ce475e8bf6c0d4d86f2b6d317011babb38769c07` | clean tracked state; one `.campaign-portability` artifact | Archived, branch retained and fast-forwarded, removed |
| `tf-ql-docs-reviewable` | `9bb24d6d5a8b54db356f82c39b4624edd4e5611a` | one tracked documentation modification | Diff archived and verified, removed |
| `tf-ql-fallback-final` | `41fa76bde8eb97541b41ee1db03daaad3fa45cbf` | clean tracked state; 7 untracked files under `benchmarks`, `tests`, and `tools` | Archived, verified, removed |
| `tf-ql-fallback-reviewable` | `5ddd23c47395863e41eb555bd865ee809479b9a4` | clean tracked state; one untracked `external` artifact | Archived, verified, removed |
| `tf-ql-runtime-reviewable` | `a1b9e11941e139753227c5df14a97af95878c09c` | clean | Removed directly |
| `tf-ql-tooling-reviewable` | `75844eac94730a5675759eced8e13ab0c291fbba` | clean | Removed directly |
| `luna-pr7-58de9d716` | `58de9d7160744bee357d197df0e85b84087b2b8e` | clean, detached; PR review checkout | Removed directly |

## Evidence preservation

Archived material is at:

`/mnt/workdrive/ZCodeProject/temporal_forge_archives/20260911/`

Each archived worktree has:

- `HEAD` containing the exact source SHA;
- `tracked.diff` containing tracked modifications;
- `staged.diff` containing staged modifications, if any;
- `status.txt` containing the original status;
- `untracked.tar.gz` containing all untracked files.

The archived untracked-file counts were verified before removal:

| Archive | Source files | Archived files |
|---|---:|---:|
| `review-baseline` | 144 | 144 |
| `review-candidate-b` | 198 | 198 |
| `tf-portability-remediation` | 1 | 1 |
| `tf-ql-docs-reviewable` | 0 | 0 |
| `tf-ql-fallback-final` | 7 | 7 |
| `tf-ql-fallback-reviewable` | 1 | 1 |

The large canonical campaign roots were not copied or deleted. They remain in
the canonical checkout and are locally excluded through `.git/info/exclude`:

- `.campaign_event_smoke`
- `.campaign_gate_trace`
- `.campaign_motion`
- `.campaign_motion_derived`
- `.campaign_motion_multiframe`
- `.candidate_capture`
- `.codex`
- `.video_agent`
- `benchmarks`
- `docs`
- `AGENTS_CODEX_LUNA.md`
- `TEMPORAL_FORGE_REPO_INTELLIGENCE.md`
- `quality_code.tar.gz`
- `temporal_forge_player-review.tar.gz`

The local exclusion is not a deletion or history rewrite; it keeps the
evidence available while allowing the canonical Git worktree to report clean.

## Canonical checkpoint

The 24 tracked canonical modifications were committed as:

```text
12bc740e481741bf1d7e6c439fe2b73dc41bc55d
chore: checkpoint pre-Expected-Food campaign state
```

The pre-checkpoint state is additionally preserved by:

```text
archive/quality-lab-vibecoder-pre-food-20260911
738ca55c6d2843c342cd8b4e66131ce5f8dddf56
```

The final canonical branch is:

```text
quality-lab-vibecoder
12bc740e481741bf1d7e6c439fe2b73dc41bc55d
```

Final `git status --porcelain` output was empty.

## Portability branch and PR #7

PR #7 is still open:

<https://github.com/Rolaand-Jayz/Temporal-Forge-Player/pull/7>

Remote PR branch:

```text
origin/portability/clean-clone-remediation
d1b5d24931f6dfe02d7838ec09ef8699dd5361fa
```

The local portability worktree originally pointed to:

```text
ce475e8bf6c0d4d86f2b6d317011babb38769c07
```

That SHA was verified as an ancestor of the remote head. The retained local
branch `portability/clean-clone-remediation` was fast-forwarded to
`d1b5d24931f6dfe02d7838ec09ef8699dd5361fa`. The worktree was then removed
after its local evidence was archived.

No merge, forced update, or history rewrite was performed.

## Final worktree state

```text
/mnt/workdrive/ZCodeProject/temporal_forge_player
  quality-lab-vibecoder @ 12bc740e481741bf1d7e6c439fe2b73dc41bc55d

/mnt/workdrive/ZCodeProject/temporal-forge-devil
  motion-campaign-devil @ 83a687aa010791aaeeca2bcbb9feda758c14a155
```

Registered worktree count: **2**.

Historical branches remain available without requiring historical worktrees.

## Readiness decision

**READY** to create the FSR Expected-Food campaign branch from:

```text
quality-lab-vibecoder
12bc740e481741bf1d7e6c439fe2b73dc41bc55d
```

The campaign branch was intentionally not created in this cleanup task.
