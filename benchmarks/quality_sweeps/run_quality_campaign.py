#!/usr/bin/env python3
"""Run an M6 campaign through isolated, dimension-preserving sweep plans.

This is the execution bridge for the corrected campaign schema. It delegates
actual frame production to the existing quality runner only when explicitly
invoked; ordinary verification remains capture-free.
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import subprocess
import sys
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# The repository is used directly rather than installed as a package.
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from benchmarks.quality_sweeps.quality_campaign_contract import (
    CampaignError,
    runner_plans,
)
from benchmarks.quality_sweeps.campaign_provenance import (
    capture_git_commit,
    stamp_result_git_commit,
)


SWEEP = Path(__file__).with_name("run_quality_sweep.py")
SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


def validate_candidate_artifacts(
    candidate_root: Path, candidate_id: str, evidence_mode: str = "visual_and_metrics"
) -> Path:
    """Reject a zero-exit child unless it produced a complete candidate run.

    The delegated sweep owns capture details, but this wrapper owns the
    campaign-level success claim.  A child that exits zero without a run,
    non-empty metrics, or non-empty representative stills is not success.
    """
    if not candidate_root.is_dir():
        raise CampaignError(f"candidate {candidate_id} returned success without an artifact directory")
    run_dirs = [path for path in candidate_root.iterdir() if path.is_dir()]
    if len(run_dirs) != 1:
        raise CampaignError(
            f"candidate {candidate_id} returned success without exactly one run artifact"
        )
    run_root = run_dirs[0]
    for name in ("results.json", "rankings.csv", "rankings.json"):
        path = run_root / name
        if not path.is_file() or path.stat().st_size == 0:
            raise CampaignError(f"candidate {candidate_id} is missing non-empty {name}")
    result_paths = sorted(
        path for path in run_root.rglob("result.json") if path.is_file()
    )
    if len(result_paths) != 1:
        raise CampaignError(
            f"candidate {candidate_id} must contain exactly one result.json; "
            f"found {len(result_paths)}"
        )
    result_path = result_paths[0]
    if result_path.stat().st_size == 0:
        raise CampaignError(f"candidate {candidate_id} is missing non-empty result.json")
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("candidateId") != candidate_id or result.get("exitCode") != 0:
        raise CampaignError(f"candidate {candidate_id} has invalid result identity or status")
    metrics = result.get("metrics")
    if not isinstance(metrics, dict) or not isinstance(metrics.get("rowCount"), int):
        raise CampaignError(f"candidate {candidate_id} is missing metric summary")
    if metrics["rowCount"] <= 0:
        raise CampaignError(f"candidate {candidate_id} has empty metric artifacts")
    csv_path = Path(result.get("csv", ""))
    if not csv_path.is_file() or csv_path.stat().st_size == 0:
        raise CampaignError(f"candidate {candidate_id} is missing non-empty quality.csv")
    if evidence_mode == "metrics_only":
        # Metrics-only campaigns deliberately retain no image payload. The
        # metric summary and exact execution provenance above remain required.
        return result_path

    stills = result.get("representativeStillPaths")
    if not isinstance(stills, list) or not stills:
        raise CampaignError(f"candidate {candidate_id} is missing representative stills")
    for still in stills:
        path = Path(still)
        if not path.is_file() or path.stat().st_size == 0:
            raise CampaignError(f"candidate {candidate_id} has an empty/missing still: {still}")
    review_assets = result.get("reviewAssets")
    if not isinstance(review_assets, list) or not review_assets:
        raise CampaignError(f"candidate {candidate_id} is missing fresh review asset metadata")
    if {asset.get("path") for asset in review_assets if isinstance(asset, dict)} != set(stills):
        raise CampaignError(f"candidate {candidate_id} review assets do not match its fresh stills")
    return result_path


def _campaign_with_execution_provenance(
    campaign: dict,
    results: list[dict],
    results_path: Path,
    git_commit: str | None,
    workers: int,
    retries: int,
) -> dict:
    """Write a derived campaign identity without mutating the source manifest."""
    if not results:
        raise CampaignError("cannot assemble campaign provenance without results")
    by_candidate = {result.get("candidateId"): result for result in results}
    if len(by_candidate) != len(results):
        raise CampaignError("campaign results contain duplicate candidate identities")
    output = copy.deepcopy(campaign)
    candidate_ids = {candidate["id"] for candidate in output["candidates"]}
    if set(by_candidate) != candidate_ids:
        raise CampaignError(
            "campaign results candidate identities do not match campaign: "
            f"results={sorted(by_candidate)}, campaign={sorted(candidate_ids)}"
        )
    execution = dict(output.get("executionProvenance") or {})
    first = results[0]
    binary = first.get("binary")
    binary_sha256 = first.get("binarySha256")
    if not isinstance(binary, str) or not isinstance(binary_sha256, str):
        raise CampaignError("campaign results are missing binary provenance")
    for result in results:
        if result.get("binary") != binary or result.get("binarySha256") != binary_sha256:
            raise CampaignError("campaign results contain mixed binary provenance")
    execution.update(
        {
            "resultsPath": str(results_path),
            "executedBinary": binary,
            "executedBinarySha256": binary_sha256,
            "gitCommit": git_commit,
            "gitCommitStatus": "recorded" if git_commit is not None else "unrecorded",
            "gitCommitAuthoritative": git_commit is not None,
            "captureWorkers": workers,
            "captureRetries": retries,
        }
    )
    if git_commit is not None:
        execution["currentGitHead"] = git_commit
    output["executionProvenance"] = execution
    for candidate in output["candidates"]:
        candidate_id = candidate["id"]
        result = by_candidate.get(candidate_id)
        if result is None:
            raise CampaignError(f"campaign results are missing candidate {candidate_id}")
        if result.get("binarySha256") != candidate.get("binarySha256"):
            raise CampaignError(f"candidate {candidate_id} binary provenance does not match campaign")
        declared_config_sha256 = candidate.get("configSha256")
        if declared_config_sha256 is not None and result.get("configSha256") != declared_config_sha256:
            raise CampaignError(f"candidate {candidate_id} config provenance does not match campaign")
        if not isinstance(result.get("configSha256"), str):
            raise CampaignError(f"candidate {candidate_id} result is missing config provenance")
        candidate["configSha256"] = result["configSha256"]
        candidate["gitCommit"] = git_commit
        candidate["reviewAssets"] = copy.deepcopy(result["reviewAssets"])
    return output


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    parser.add_argument("--continue-on-error", action="store_true")
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, int(os.environ.get("TFORGE_CAPTURE_WORKERS", "2"))),
        help="Maximum independent candidate campaigns to run concurrently (default: 2).",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=1,
        help="Isolated retry attempts passed to each candidate sweep (default: 1).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers < 1 or args.retries < 0:
        raise CampaignError("workers must be >= 1 and retries must be >= 0")
    with args.campaign.open("r", encoding="utf-8") as stream:
        campaign = json.load(stream)
    plans = runner_plans(campaign)
    git_commit = capture_git_commit(ROOT)
    if args.output_root.exists() and any(args.output_root.iterdir()):
        raise CampaignError(f"output root is not empty: {args.output_root}")
    args.output_root.mkdir(parents=True, exist_ok=True)
    plan_root = args.output_root / "plans"
    plan_root.mkdir()
    failures = 0
    campaign_results: list[dict] = []
    prepared: list[tuple[int, str, Path, Path]] = []
    for plan in plans:
        candidate_id = plan["experiments"][0]["id"]
        if not SAFE_ID.fullmatch(candidate_id):
            raise CampaignError(f"unsafe candidate id: {candidate_id}")
        plan["experiments"][0]["config"] = str(
            (ROOT / plan["experiments"][0]["config"]).resolve()
        )
        plan_path = plan_root / f"{candidate_id}.json"
        plan_path.write_text(json.dumps(plan, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        candidate_root = args.output_root / candidate_id
        prepared.append((len(prepared), candidate_id, plan_path, candidate_root))

    def run_plan(item: tuple[int, str, Path, Path]) -> tuple[int, dict | None, str | None]:
        index, candidate_id, plan_path, candidate_root = item
        command = [
            sys.executable,
            str(SWEEP),
            "--manifest", str(plan_path),
            "--binary", str(args.binary),
            "--output-root", str(candidate_root),
            "--tag-prefix", f"m6-{candidate_id}",
            "--retries", str(args.retries),
        ]
        result = subprocess.run(command, cwd=ROOT, check=False)
        if result.returncode != 0:
            return index, None, f"candidate {candidate_id} sweep exited {result.returncode}"
        try:
            result_path = validate_candidate_artifacts(
                candidate_root, candidate_id, campaign.get("evidenceMode", "visual_and_metrics")
            )
            result_data = json.loads(result_path.read_text(encoding="utf-8"))
            stamped_result = stamp_result_git_commit(result_data, git_commit)
            result_path.write_text(
                json.dumps(stamped_result, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            return index, stamped_result, None
        except (CampaignError, OSError, json.JSONDecodeError) as error:
            return index, None, str(error)

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run_plan, item) for item in prepared]
        completed = [future.result() for future in as_completed(futures)]
    for _, result, error in sorted(completed):
        if error is not None:
            failures += 1
            if not args.continue_on_error:
                return 2
        elif result is not None:
            campaign_results.append(result)
    if failures:
        return 2
    results_path = args.output_root / "campaign-results.json"
    results_path.write_text(
        json.dumps(campaign_results, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    derived_campaign = _campaign_with_execution_provenance(
        campaign,
        campaign_results,
        results_path,
        git_commit,
        args.workers,
        args.retries,
    )
    (args.output_root / "campaign.json").write_text(
        json.dumps(derived_campaign, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CampaignError, OSError, json.JSONDecodeError) as error:
        print(f"quality campaign error: {error}", file=sys.stderr)
        raise SystemExit(2)
