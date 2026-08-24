"""Strict provenance checks for an executed M6 campaign evidence set.

This boundary compares recorded evidence with the files that were actually
named by the runner.  It deliberately treats an omitted Git commit as
unrecorded provenance; the current checkout's HEAD is not substituted for it.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

from .quality_campaign_contract import CampaignError, validate_campaign


class ProvenanceError(CampaignError):
    """Raised when campaign evidence is stale, inconsistent, or ambiguous."""


_SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")
_COMMIT = re.compile(r"^[0-9a-fA-F]{7,64}$")


def capture_git_commit(repo_root: Path) -> str | None:
    """Read the checkout commit without substituting it when Git is unavailable.

    This is capture-free metadata collection.  ``None`` is an explicit
    unavailable state; callers must not replace it with a different observed
    ref or with the current process checkout by assumption.
    """
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            cwd=repo_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if completed.returncode != 0:
        return None
    if not isinstance(completed.stdout, str):
        return None
    commit = completed.stdout.strip()
    if not _COMMIT.fullmatch(commit):
        return None
    try:
        status = subprocess.run(
            ["git", "-C", str(repo_root), "status", "--porcelain"],
            cwd=repo_root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if status.returncode != 0 or not isinstance(status.stdout, str) or status.stdout.strip():
        return None
    return commit.lower()


def stamp_result_git_commit(result: Mapping[str, Any], git_commit: str | None) -> dict[str, Any]:
    """Copy a result and stamp only proven campaign-level Git metadata."""
    stamped = dict(result)
    if git_commit is not None and not _COMMIT.fullmatch(git_commit):
        raise ProvenanceError(f"git commit is not a commit id: {git_commit!r}")
    stamped["gitCommit"] = git_commit
    stamped["gitCommitStatus"] = "recorded" if git_commit is not None else "unrecorded"
    stamped["gitCommitAuthoritative"] = git_commit is not None
    return stamped


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise ProvenanceError(f"cannot hash provenance file {path}: {error}") from error
    return digest.hexdigest()


def _path(value: Any, name: str, repo_root: Path) -> Path:
    if not isinstance(value, str) or not value:
        raise ProvenanceError(f"{name} must be a non-empty path")
    candidate = Path(value)
    return candidate if candidate.is_absolute() else (repo_root / candidate).resolve()


def _digest(value: Any, name: str) -> str:
    if not isinstance(value, str) or not _SHA256.fullmatch(value):
        raise ProvenanceError(f"{name} must be a SHA-256 digest")
    return value.lower()


def _commit(value: Any, name: str) -> str:
    if not isinstance(value, str) or not _COMMIT.fullmatch(value):
        raise ProvenanceError(f"{name} must be a commit id")
    return value.lower()


def validate_execution_provenance(
    campaign: Mapping[str, Any],
    results: Sequence[Any],
    repo_root: Path,
) -> None:
    """Require exact result, campaign, and on-disk binary/config provenance.

    The M6 retry used one executable for all candidates.  The manifest must
    therefore identify that executable once and every result must name the
    same path and digest.  Git may be explicitly marked ``unrecorded`` but a
    checkout HEAD may not be silently promoted to execution provenance.
    """

    try:
        validate_campaign(campaign)
    except CampaignError as error:
        raise ProvenanceError(f"campaign is not valid: {error}") from error

    execution = campaign.get("executionProvenance")
    if not isinstance(execution, Mapping):
        raise ProvenanceError("campaign.executionProvenance is required")
    binary_path = _path(execution.get("executedBinary"), "executionProvenance.executedBinary", repo_root)
    binary_digest = _digest(
        execution.get("executedBinarySha256"),
        "executionProvenance.executedBinarySha256",
    )
    if not binary_path.is_file():
        raise ProvenanceError(f"executed binary does not exist: {binary_path}")
    actual_binary_digest = _sha256(binary_path)
    if actual_binary_digest != binary_digest:
        raise ProvenanceError(
            "executed binary hash is stale: "
            f"manifest={binary_digest}, actual={actual_binary_digest}, path={binary_path}"
        )
    if execution.get("buildFastBinary") is not None:
        build_fast_path = _path(
            execution.get("buildFastBinary"),
            "executionProvenance.buildFastBinary",
            repo_root,
        )
        build_fast_digest = _digest(
            execution.get("buildFastBinarySha256"),
            "executionProvenance.buildFastBinarySha256",
        )
        if not build_fast_path.is_file():
            raise ProvenanceError(f"observed build-fast binary does not exist: {build_fast_path}")
        actual_build_fast_digest = _sha256(build_fast_path)
        if actual_build_fast_digest != build_fast_digest:
            raise ProvenanceError(
                "observed build-fast binary hash is stale: "
                f"manifest={build_fast_digest}, actual={actual_build_fast_digest}, path={build_fast_path}"
            )

    git_status = execution.get("gitCommitStatus")
    if git_status not in {"recorded", "unrecorded"}:
        raise ProvenanceError(
            "executionProvenance.gitCommitStatus must be recorded or unrecorded"
        )
    manifest_git = execution.get("gitCommit")
    if git_status == "recorded":
        recorded_git = _commit(manifest_git, "executionProvenance.gitCommit")
    else:
        if manifest_git is not None or execution.get("gitCommitAuthoritative") is not False:
            raise ProvenanceError(
                "unrecorded Git provenance must use gitCommit=null and "
                "gitCommitAuthoritative=false"
            )
        recorded_git = None
    current_head = execution.get("currentGitHead")
    _commit(current_head, "executionProvenance.currentGitHead")

    candidates = {candidate["id"]: candidate for candidate in campaign["candidates"]}
    if not isinstance(results, Sequence) or isinstance(results, (str, bytes)) or not results:
        raise ProvenanceError("campaign results must be a non-empty list")
    seen: set[str] = set()
    for row_number, result in enumerate(results, start=1):
        if not isinstance(result, Mapping):
            raise ProvenanceError(f"campaign result {row_number} must be an object")
        candidate_id = result.get("candidateId")
        if candidate_id not in candidates:
            raise ProvenanceError(f"campaign result {row_number} names unknown candidate: {candidate_id!r}")
        if candidate_id in seen:
            raise ProvenanceError(f"campaign results contain duplicate candidate: {candidate_id}")
        seen.add(candidate_id)
        candidate = candidates[candidate_id]
        if result.get("exitCode") != 0:
            raise ProvenanceError(f"candidate {candidate_id} did not complete successfully")

        result_binary = _path(result.get("binary"), f"result {candidate_id}.binary", repo_root)
        if result_binary != binary_path:
            raise ProvenanceError(
                f"candidate {candidate_id} result names a different binary: "
                f"{result_binary} != {binary_path}"
            )
        result_binary_digest = _digest(result.get("binarySha256"), f"result {candidate_id}.binarySha256")
        if result_binary_digest != binary_digest or result_binary_digest != _digest(
            candidate.get("binarySha256"), f"candidate {candidate_id}.binarySha256"
        ):
            raise ProvenanceError(f"candidate {candidate_id} binary provenance is not equal")

        config_path = _path(result.get("configSource"), f"result {candidate_id}.configSource", repo_root)
        declared_config_path = _path(candidate.get("configPath"), f"candidate {candidate_id}.configPath", repo_root)
        if config_path != declared_config_path:
            raise ProvenanceError(f"candidate {candidate_id} config path provenance is not equal")
        if not config_path.is_file():
            raise ProvenanceError(f"candidate {candidate_id} config does not exist: {config_path}")
        result_config_digest = _digest(result.get("configSha256"), f"result {candidate_id}.configSha256")
        candidate_config_digest = _digest(candidate.get("configSha256"), f"candidate {candidate_id}.configSha256")
        actual_config_digest = _sha256(config_path)
        if result_config_digest != candidate_config_digest or result_config_digest != actual_config_digest:
            raise ProvenanceError(f"candidate {candidate_id} config provenance is not equal")

        result_git = result.get("gitCommit")
        if git_status == "recorded":
            if _commit(result_git, f"result {candidate_id}.gitCommit") != recorded_git:
                raise ProvenanceError(f"candidate {candidate_id} Git provenance is not equal")
            if _commit(candidate.get("gitCommit"), f"candidate {candidate_id}.gitCommit") != recorded_git:
                raise ProvenanceError(f"candidate {candidate_id} manifest Git provenance is not equal")
        elif result_git is not None or candidate.get("gitCommit") is not None:
            raise ProvenanceError(
                f"candidate {candidate_id} has Git provenance despite an unrecorded campaign"
            )

    missing = sorted(set(candidates) - seen)
    if missing:
        raise ProvenanceError(f"campaign results are missing candidates: {missing}")
