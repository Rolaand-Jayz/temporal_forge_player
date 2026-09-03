#!/usr/bin/env python3
"""Populate campaign evidence and the portable review harness together.

Each pair is a transaction: renderer arms are captured serially, source-based
controls are created from the matched decoded input frame, then every method
for the pair is exported and dimension-checked.  A pair is never marked done
until all 23 method IDs have files for all four scenes. Planning is the safe
default; live capture requires the explicit ``--execute`` gate.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from benchmarks.quality_sweeps.trackmania_guard import DEFAULT_GAME_PATTERNS, running_games
from tools.export_review_image import export_review_image

CAPTURE_PLAN_PATH = ROOT / "benchmarks/quality_sweeps/quality_campaign_capture_plan.json"


def load_capture_plan(path: Path = CAPTURE_PLAN_PATH) -> dict[str, object]:
    """Load and fail closed on the checked-in campaign/harness contract."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema") != "temporal_forge.quality_campaign_capture_plan.v1":
        raise ValueError(f"unsupported capture plan schema: {path}")
    pairs = data.get("pairs")
    scenes = data.get("scenes")
    scales = data.get("scales")
    if not isinstance(pairs, list) or not isinstance(scenes, list) or not isinstance(scales, list):
        raise ValueError(f"capture plan is missing pairs/scenes/scales: {path}")
    expected = {
        (360, 480), (360, 720), (360, 1080),
        (480, 720), (480, 1080), (480, 1440),
        (720, 1080), (720, 1440), (720, 2160),
        (1080, 1440), (1080, 2160),
    }
    actual = {(item.get("inputHeight"), item.get("outputHeight")) for item in pairs}
    if actual != expected or len(pairs) != len(expected):
        raise ValueError(f"capture plan must contain exactly the 11 legal upscale pairs: {path}")
    if 540 in {value for pair in actual for value in pair}:
        raise ValueError("540p is not part of the active quality campaign")
    if len(scenes) != 4 or len(set(scenes)) != 4:
        raise ValueError("capture plan must contain four unique campaign scenes")
    if scales != [2.0, 2.25, 2.5, 2.75, 3.0]:
        raise ValueError("capture plan scale set does not match the campaign contract")
    expected_dimensions = {
        360: "640x360", 480: "854x480", 720: "1280x720", 1080: "1920x1080",
        1440: "2560x1440", 2160: "3840x2160",
    }
    for item in pairs:
        if item.get("uses") != ["quality_campaign", "review_harness"]:
            raise ValueError("each planned pair must explicitly name both evidence consumers")
        input_height = item["inputHeight"]
        output_height = item["outputHeight"]
        if item.get("source") != expected_dimensions[input_height]:
            raise ValueError(f"source dimensions do not match {input_height}p")
        if item.get("delivery") != expected_dimensions[output_height]:
            raise ValueError(f"delivery dimensions do not match {output_height}p")
    required_metadata = {
        "campaignId": "quality-campaign-20260902",
        "status": "ready_not_started",
        "frame": 48,
        "casStrength": 0.2,
        "profile": "AMD_SEMANTIC_BASELINE",
    }
    for key, value in required_metadata.items():
        if data.get(key) != value:
            raise ValueError(f"capture plan {key} must be {value!r}")
    expected_arms = [
        {"id": "resolve_cas20", "rendererCas": 0.2, "afterDownsamplingCas": 0.0,
         "label": "CAS 0.20 before downsampling"},
        {"id": "external_post_cas20", "rendererCas": 0.0, "afterDownsamplingCas": 0.2,
         "label": "CAS 0.20 after downsampling"},
        {"id": "no_cas", "rendererCas": 0.0, "afterDownsamplingCas": 0.0,
         "label": "No CAS sharpening"},
    ]
    if data.get("downsamplingArms") != expected_arms:
        raise ValueError("capture plan must define the three independent downsampling CAS arms")
    return data


CAPTURE_PLAN = load_capture_plan()
SCENES = tuple(str(scene) for scene in CAPTURE_PLAN["scenes"])
SCALES = tuple(float(scale) for scale in CAPTURE_PLAN["scales"])
CAS_PLACEMENTS = ("pre", "post", "none")
CAS_METHOD_SUFFIX = {"pre": "resolve_cas20", "post": "external_post_cas20", "none": "no_cas"}
PAIRS = tuple(
    (int(item["inputHeight"]), str(item["source"]),
     int(item["outputHeight"]), str(item["delivery"]))
    for item in CAPTURE_PLAN["pairs"]
)
METHODS = (
    "current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
    *(f"fsr_{int(scale * 100):03d}x_downsample_"
      f"{CAS_METHOD_SUFFIX[placement]}"
      for scale in SCALES for placement in CAS_PLACEMENTS),
    "fsr_nativeaa_downsample_resolve_cas20", "fsr_nativeaa_downsample_external_post_cas20",
    "fsr_nativeaa_downsample_no_cas", "conventional_lanczos", "conventional_bicubic",
)


def now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def image_record(path: Path) -> dict[str, object]:
    with path.open("rb") as stream:
        if stream.read(8) != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"not a PNG: {path}")
        stream.seek(16)
        width, height = struct.unpack(">II", stream.read(8))
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"path": str(path), "bytes": path.stat().st_size, "width": width,
            "height": height, "sha256": digest, "mtime": dt.datetime.fromtimestamp(
                path.stat().st_mtime, dt.timezone.utc).isoformat()}


def catalog_record(path: Path, *, scene: str, method: str, input_height: int,
                   output_height: int, view: str,
                   provenance: dict[str, object] | None = None) -> dict[str, object]:
    record = image_record(path)
    record.update({"scene": scene, "method": method, "input": input_height,
                   "output": output_height, "view": view,
                   "frame": int(CAPTURE_PLAN["frame"]),
                   "campaign_id": CAPTURE_PLAN["campaignId"],
                   "validation": "validated_experiment"})
    if provenance:
        record.update(provenance)
    return record


def experiment_provenance(scene_root: Path, *, category: str,
                          alias_of: str | None = None) -> dict[str, object]:
    """Load identity from the validated arm record, never from its filename."""
    record_path = scene_root / "experiment.json"
    if not record_path.is_file():
        return {"pipeline_category": category, "validation": "external_control"}
    data = json.loads(record_path.read_text(encoding="utf-8"))
    if data.get("status") != "complete":
        raise SystemExit(f"cannot publish incomplete experiment provenance: {record_path}")
    result = {
        "experiment_id": data.get("experiment_id"),
        "profile": data.get("profile"),
        "pipeline_category": category,
        "cas_strength": data.get("cas_strength"),
        "cas_placement": data.get("cas_placement"),
        "cas_stage": data.get("runtime_trace", {}).get("cas_stage"),
        "jitter_mode": data.get("runtime_trace", {}).get("jitter_mode"),
        "binary_sha256": data.get("binary_sha256"),
        "config_sha256": data.get("config_sha256"),
    }
    if alias_of:
        result["alias_of"] = alias_of
    return result


def write_catalog(harness: Path, records: list[dict[str, object]]) -> None:
    """Publish only assets from a completed, validated pair."""
    catalog_path = harness / "catalog.json"
    existing: dict[str, object] = {"schema": "temporal_forge.review_catalog.v1", "assets": []}
    if catalog_path.is_file():
        try:
            parsed = json.loads(catalog_path.read_text(encoding="utf-8"))
            if isinstance(parsed, dict) and isinstance(parsed.get("assets"), list):
                existing = parsed
        except (OSError, json.JSONDecodeError):
            existing = {"schema": "temporal_forge.review_catalog.v1", "assets": []}
    by_key = {
        (item.get("scene"), item.get("method"), item.get("input"), item.get("output"), item.get("view")): item
        for item in existing["assets"]
        if isinstance(item, dict) and item.get("campaign_id") == CAPTURE_PLAN["campaignId"]
    }
    for item in records:
        key = (item["scene"], item["method"], item["input"], item["output"], item["view"])
        catalog_item = dict(item)
        catalog_item["path"] = "images/" + Path(str(item["path"])).name
        by_key[key] = catalog_item
    assets = sorted(by_key.values(), key=lambda item: (
        str(item.get("scene")), int(item.get("input", 0)), int(item.get("output", 0)),
        str(item.get("method")), str(item.get("view"))))
    atomic_json(catalog_path, {
        "schema": "temporal_forge.review_catalog.v2",
        "campaign": CAPTURE_PLAN["campaignId"],
        "validation": "entries are emitted only after pair validation",
        "assets": assets,
    })
    catalog_js = harness / "catalog.js"
    temporary = catalog_js.with_name(f".{catalog_js.name}.{os.getpid()}.tmp")
    browser_plan = {
        "campaignId": CAPTURE_PLAN["campaignId"],
        "status": CAPTURE_PLAN["status"],
        "frame": CAPTURE_PLAN["frame"],
        "downsamplingArms": CAPTURE_PLAN["downsamplingArms"],
        "pairs": [
            {"input": item["inputHeight"], "output": item["outputHeight"]}
            for item in CAPTURE_PLAN["pairs"]
        ],
    }
    temporary.write_text(
        "window.__tforgeCampaign = " + json.dumps(browser_plan) + ";\n" +
        "window.__tforgeCatalog = " + json.dumps({"assets": assets}) + ";\n",
        encoding="utf-8",
    )
    os.replace(temporary, catalog_js)


def csv_record(path: Path) -> dict[str, object]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    return {"path": str(path), "bytes": path.stat().st_size, "rows": len(rows),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "mtime": dt.datetime.fromtimestamp(path.stat().st_mtime,
                                                 dt.timezone.utc).isoformat()}


class PausingRunner:
    """Run capture commands without imposing a game-process pause gate."""
    def __init__(self, artifact_root: Path, patterns: tuple[str, ...], allow_games: tuple[str, ...], poll_seconds: float = 2.0) -> None:
        self.patterns = patterns
        self.allow_games = allow_games
        self.poll_seconds = poll_seconds
        self.log = artifact_root / "game_guard_events.jsonl"
        self.log.parent.mkdir(parents=True, exist_ok=True)
        if self.allow_games:
            self.record("allow_game_exception", {"patterns": list(self.allow_games)})

    def games(self) -> list[dict[str, str]]:
        return running_games(self.patterns, self.allow_games)

    def record(self, event: str, details: object) -> None:
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"timestamp": now(), "event": event, "games": details}) + "\n")

    def wait_until_clear(self, label: str) -> None:
        # Kept as a compatibility hook for callers; capture work is never
        # paused or suspended based on game processes.
        return

    def run(self, command: list[str], label: str, cwd: Path = ROOT) -> None:
        started = time.monotonic()
        print(f"[{now()}] START {label}", flush=True)
        self.record("command_start", {"label": label, "command": command})
        proc = subprocess.Popen(command, cwd=cwd, start_new_session=True)
        try:
            proc.wait()
            if proc.returncode:
                raise subprocess.CalledProcessError(proc.returncode, command)
            self.record("command_complete", {"label": label, "returncode": proc.returncode})
            print(f"[{now()}] DONE  {label} ({time.monotonic() - started:.1f}s)", flush=True)
        except BaseException:
            if proc.poll() is None:
                proc.terminate()
                proc.wait()
            raise


def filename(scene: str, input_height: int, method: str, output_height: int) -> str:
    return (f"scene-{scene}__frame-0048__in-{input_height}p__method-{method}"
            f"__out-{output_height}p.png")


def native_filename(scene: str, input_height: int, method: str, output_height: int) -> str:
    return filename(scene, input_height, method, output_height).removesuffix(".png") + "__native.png"


def has_native_output(method: str) -> bool:
    return method.startswith("fsr_") and method != "fsr_direct_cas20"


def export(runner: PausingRunner, source: Path, scene: str, input_height: int, method: str, output_height: int, harness: Path) -> None:
    export_review_image(
        source, scene=scene, frame=f"{int(CAPTURE_PLAN['frame']):04d}",
        input_height=input_height, method=method, output_height=output_height,
        root=harness,
    )


def export_native(runner: PausingRunner, source: Path, scene: str, input_height: int,
                  method: str, output_height: int, harness: Path) -> None:
    output = harness / "images" / native_filename(scene, input_height, method, output_height)
    output.parent.mkdir(parents=True, exist_ok=True)
    image_record(source)
    shutil.copyfile(source, output)


def source_raw(scene_root: Path) -> Path:
    candidates = sorted((scene_root / "quality_frames").glob("*_gpu_raw.png"))
    if len(candidates) != 1:
        raise SystemExit(f"expected one matched decoded source frame in {scene_root}, got {candidates}")
    return candidates[0]


def native_output(scene_root: Path) -> Path:
    """Return the native FSR output recorded by run_fsr_supersampling."""
    raw_csv = scene_root / "raw.csv"
    with raw_csv.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1 or not rows[0].get("output_path"):
        raise SystemExit(f"expected one native output record in {raw_csv}")
    output = Path(rows[0]["output_path"])
    if not output.is_file():
        raise SystemExit(f"native FSR output is missing: {output}")
    return output


def validate_resume_pair(pair_root: Path, input_height: int, output_height: int,
                         harness: Path, player: Path) -> None:
    """Fail closed when a completion marker is stale or semantically old."""
    marker = pair_root / "harness_pair_complete.json"
    try:
        data = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot validate resume marker {marker}: {error}") from error
    if data.get("schemaVersion") != 3 or data.get("runnerVersion") != 3:
        raise SystemExit(f"stale completion marker schema: {marker}")
    if data.get("input") != input_height or data.get("output") != output_height:
        raise SystemExit(f"completion marker pair mismatch: {marker}")
    if data.get("scenes") != list(SCENES) or data.get("methods") != list(METHODS):
        raise SystemExit(f"completion marker matrix mismatch: {marker}")
    expected_assets = len(SCENES) * len(METHODS)
    expected_native = sum(has_native_output(method) for method in METHODS) * len(SCENES)
    if data.get("assets") != expected_assets or data.get("native_assets") != expected_native:
        raise SystemExit(f"completion marker asset count mismatch: {marker}")
    required_record_keys = ("path", "bytes", "width", "height", "sha256", "mtime")
    for item in data.get("assets_detail", []) + data.get("native_assets_detail", []):
        path = Path(item.get("path", ""))
        current = image_record(path) if path.is_file() else {}
        if not path.is_file() or any(current.get(key) != item.get(key) for key in required_record_keys):
            raise SystemExit(f"completion marker artifact hash/dimensions mismatch: {path}")
    expected_binary = hashlib.sha256(player.read_bytes()).hexdigest()
    config_path = ROOT / "benchmarks/video_corpus/benchmark_settings.json"
    expected_config = hashlib.sha256(config_path.read_bytes()).hexdigest()
    seen_experiments: set[str] = set()
    for arm in ("fsr_pre", "fsr_post", "fsr_none", "nativeaa_pre", "nativeaa_post", "nativeaa_none"):
        csv_path = pair_root / f"{arm}.csv"
        if not csv_path.is_file():
            raise SystemExit(f"completion marker CSV is missing: {csv_path}")
        with csv_path.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        expected_rows = len(SCENES) if arm.startswith("nativeaa_") else len(SCALES) * len(SCENES)
        if len(rows) != expected_rows:
            raise SystemExit(f"completion marker CSV row count mismatch: {csv_path}")
        for row in rows:
            trace = Path(row.get("runtime_trace_path", ""))
            record = trace.parent / "experiment.json"
            if not trace.is_file() or not record.is_file():
                raise SystemExit(f"missing runtime provenance for {csv_path}: {trace}")
            try:
                trace_data = json.loads(trace.read_text(encoding="utf-8"))
                record_data = json.loads(record.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise SystemExit(f"invalid runtime provenance for {csv_path}: {error}") from error
            experiment_id = record_data.get("experiment_id")
            if not isinstance(experiment_id, str) or not experiment_id or experiment_id in seen_experiments:
                raise SystemExit(f"duplicate/missing experiment identity in {record}")
            seen_experiments.add(experiment_id)
            if row.get("run_id") != experiment_id or trace_data.get("run_id") != experiment_id:
                raise SystemExit(f"experiment identity mismatch in {record}")
            if trace_data.get("binary_sha256") != expected_binary or record_data.get("binary_sha256") != expected_binary:
                raise SystemExit(f"binary provenance mismatch in {record}")
            if (trace_data.get("config_sha256") != expected_config or
                    record_data.get("config_sha256") != expected_config):
                raise SystemExit(f"configuration provenance mismatch in {record}")
            if (trace_data.get("quality_profile") != "AMD_SEMANTIC_BASELINE" or
                    record_data.get("profile") != "AMD_SEMANTIC_BASELINE"):
                raise SystemExit(f"quality profile mismatch in {record}")
            if row.get("runtime_trace_sha256") != hashlib.sha256(trace.read_bytes()).hexdigest():
                raise SystemExit(f"runtime trace hash mismatch in {trace}")
            output = Path(record_data.get("output_artifact", ""))
            if record_data.get("output_retained") is False:
                if not isinstance(record_data.get("output_sha256"), str) or len(record_data["output_sha256"]) != 64:
                    raise SystemExit(f"data-only output hash missing in {record}")
            elif (not output.is_file() or
                  record_data.get("output_sha256") != hashlib.sha256(output.read_bytes()).hexdigest()):
                raise SystemExit(f"output provenance mismatch in {record}")


def validate_resume_data_only_pair(pair_root: Path, input_height: int,
                                   output_height: int, player: Path) -> None:
    """Validate a completed pair whose derived image payloads were pruned."""
    marker = pair_root / "harness_pair_complete.json"
    try:
        data = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot validate data-only marker {marker}: {error}") from error
    if data.get("data_only") is not True or data.get("input") != input_height or data.get("output") != output_height:
        raise SystemExit(f"data-only completion marker mismatch: {marker}")
    if data.get("scenes") != list(SCENES) or data.get("scales") != list(SCALES):
        raise SystemExit(f"data-only completion matrix mismatch: {marker}")
    expected_binary = hashlib.sha256(player.read_bytes()).hexdigest()
    expected_config = hashlib.sha256((ROOT / "benchmarks/video_corpus/benchmark_settings.json").read_bytes()).hexdigest()
    records = data.get("csv_records")
    if not isinstance(records, list) or len(records) != 6:
        raise SystemExit(f"data-only CSV record count mismatch: {marker}")
    seen_experiments: set[str] = set()
    for item in records:
        path = Path(str(item.get("path", "")))
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item.get("sha256"):
            raise SystemExit(f"data-only CSV provenance mismatch: {path}")
        with path.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        expected_rows = len(SCENES) if path.name.startswith("nativeaa_") else len(SCALES) * len(SCENES)
        if len(rows) != expected_rows:
            raise SystemExit(f"data-only CSV row count mismatch: {path}")
        for row in rows:
            trace = Path(row.get("runtime_trace_path", ""))
            record = trace.parent / "experiment.json"
            if not trace.is_file() or not record.is_file():
                raise SystemExit(f"missing data-only runtime provenance: {trace}")
            trace_data = json.loads(trace.read_text(encoding="utf-8"))
            record_data = json.loads(record.read_text(encoding="utf-8"))
            experiment_id = record_data.get("experiment_id")
            if not isinstance(experiment_id, str) or not experiment_id or experiment_id in seen_experiments:
                raise SystemExit(f"duplicate/missing data-only experiment identity: {record}")
            seen_experiments.add(experiment_id)
            if row.get("run_id") != experiment_id or trace_data.get("run_id") != experiment_id:
                raise SystemExit(f"data-only experiment identity mismatch: {record}")
            if (trace_data.get("binary_sha256") != expected_binary or
                    record_data.get("binary_sha256") != expected_binary):
                raise SystemExit(f"data-only binary provenance mismatch: {record}")
            if (trace_data.get("config_sha256") != expected_config or
                    record_data.get("config_sha256") != expected_config):
                raise SystemExit(f"data-only configuration provenance mismatch: {record}")
            if record_data.get("output_retained") is not False or not record_data.get("output_sha256"):
                raise SystemExit(f"data-only output retention contract mismatch: {record}")
    expected_experiments = (3 * len(SCALES) * len(SCENES)) + (3 * len(SCENES))
    if len(seen_experiments) != expected_experiments:
        raise SystemExit(f"data-only experiment count mismatch: {marker}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build-fast/temporal_forge_player"))
    parser.add_argument("--artifact-root", type=Path,
                        default=Path("benchmarks/quality_sweeps/quality_campaign_capture"))
    parser.add_argument("--harness-root", type=Path, default=Path("review_harness"))
    parser.add_argument("--only", action="append", metavar="INPUTxOUTPUT",
                        help="limit this invocation to selected pair(s); repeatable")
    parser.add_argument("--execute", action="store_true",
                        help="start live capture; without this flag only the plan is printed")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--data-only", action="store_true",
                        help="recapture campaign CSV/raw data without rewriting harness images")
    parser.add_argument("--allow-game", action="append", default=[], metavar="PATTERN",
                        help="exclude a matching game from provenance observations; repeatable")
    parser.add_argument("--game-pattern", action="append", default=[], metavar="PATTERN",
                        help="additional game process pattern; repeatable")
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    args = parser.parse_args()
    legal_pair_names = {f"{input_height}x{output_height}"
                        for input_height, _, output_height, _ in PAIRS}
    requested_pairs = set(args.only or legal_pair_names)
    unknown_pairs = requested_pairs - legal_pair_names
    if unknown_pairs:
        parser.error(f"unknown resolution pair(s): {', '.join(sorted(unknown_pairs))}")
    selected_pairs = [pair for pair in PAIRS if f"{pair[0]}x{pair[2]}" in requested_pairs]
    plan_summary = {
        "schema": "temporal_forge.quality_campaign_capture_preview.v1",
        "campaign": CAPTURE_PLAN["campaignId"],
        "mode": "execute" if args.execute else "plan_only",
        "capture_started": False,
        "pairs": [f"{pair[0]}->{pair[2]}" for pair in selected_pairs],
        "pair_count": len(selected_pairs),
        "scenes_per_pair": len(SCENES),
        "methods_per_scene": len(METHODS),
        "renderer_arms_per_pair": 6,
        "expected_player_launches_per_pair": 72,
        "consumers": ["quality_campaign", "review_harness"],
    }
    print(json.dumps(plan_summary, indent=2))
    if not args.execute:
        return 0
    player = args.player.resolve()
    if not player.is_file() or not os.access(player, os.X_OK):
        raise SystemExit(f"capture player is missing or not executable: {player}")
    tracked_dirty = (
        subprocess.run(["git", "-C", str(ROOT), "diff", "--quiet"], check=False).returncode != 0 or
        subprocess.run(["git", "-C", str(ROOT), "diff", "--cached", "--quiet"], check=False).returncode != 0
    )
    if tracked_dirty:
        raise SystemExit("live capture requires a committed tracked worktree")
    # A data-only campaign retains the measurements and provenance but does
    # not retain the rendered payloads. Image-producing harness runs must opt
    # into retention explicitly through this branch.
    os.environ["TFORGE_PRESERVE_SPATIAL_IMAGES"] = "0" if args.data_only else "1"
    artifacts = args.artifact_root.resolve(); harness = args.harness_root.resolve()
    patterns = tuple(DEFAULT_GAME_PATTERNS) + tuple(args.game_pattern)
    runner = PausingRunner(artifacts, patterns, tuple(args.allow_game), args.poll_seconds)
    campaign_manifest = artifacts / "campaign_manifest.json"
    run_capture = ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py"
    for pair_index, (input_height, source, output_height, final) in enumerate(selected_pairs, 1):
        stem = f"resolution_{input_height}_to_{output_height}"
        print(
            f"[{now()}] ROUTE {pair_index}/{len(selected_pairs)} START "
            f"{input_height}p -> {output_height}p",
            flush=True,
        )
        pair_root = artifacts / stem
        marker = pair_root / "harness_pair_complete.json"
        pair_started = now()
        if args.resume and marker.is_file():
            if args.data_only:
                validate_resume_data_only_pair(pair_root, input_height, output_height, args.player.resolve())
            else:
                validate_resume_pair(pair_root, input_height, output_height, harness, args.player.resolve())
            print(f"Skipping complete {stem}")
            continue
        runner.wait_until_clear(stem)
        roots = {}
        for placement in CAS_PLACEMENTS:
            runner.wait_until_clear(f"{stem}/{placement}")
            root = pair_root / f"fsr_{placement}"
            csv_path = pair_root / f"fsr_{placement}.csv"
            command = [sys.executable, str(run_capture), "--player", str(args.player.resolve()),
                       "--repo", str(ROOT), "--artifact-root", str(root), "--output-csv", str(csv_path),
                       "--reference-cache", str(pair_root / "shared_references"),
                       "--final", final, "--source", source, "--cas-strength", "0.20",
                       "--cas-placement", placement]
            runner.run(command, f"capture {stem}/{placement}")
            roots[placement] = root
        native_roots = {}
        native_csvs = {}
        for placement in CAS_PLACEMENTS:
            native_root = pair_root / f"nativeaa_{placement}"
            native_csv = pair_root / f"nativeaa_{placement}.csv"
            runner.wait_until_clear(f"{stem}/nativeaa_{placement}")
            command = [sys.executable, str(run_capture), "--player", str(args.player.resolve()),
                       "--repo", str(ROOT), "--artifact-root", str(native_root), "--output-csv", str(native_csv),
                       "--reference-cache", str(pair_root / "shared_references"),
                       "--final", final, "--source", source, "--cas-strength", "0.20",
                       "--cas-placement", placement, "--preset", "NativeAA"]
            runner.run(command, f"capture {stem}/nativeaa_{placement}")
            native_roots[placement] = native_root
            native_csvs[placement] = native_csv
        if args.data_only:
            csv_paths = [pair_root / f"fsr_{placement}.csv" for placement in CAS_PLACEMENTS]
            csv_paths.extend(native_csvs.values())
            records = [csv_record(path) for path in csv_paths]
            expected_rows = {
                path.name: (len(SCENES) if path.name.startswith("nativeaa_")
                            else len(SCALES) * len(SCENES))
                for path in csv_paths
            }
            if any(record["rows"] != expected_rows[Path(str(record["path"])).name]
                   for record in records):
                raise SystemExit(f"{stem} data incomplete: arm row count mismatch")
            completed = now()
            marker_data = {"input": input_height, "output": output_height,
                           "scenes": list(SCENES), "scales": list(SCALES),
                           "arms": ([f"fsr_{placement}" for placement in CAS_PLACEMENTS] +
                                    [f"nativeaa_{placement}" for placement in CAS_PLACEMENTS]),
                           "data_only": True, "completed_at": completed,
                           "guard_log": str(runner.log), "csv_records": records}
            atomic_json(marker, marker_data)
            history = []
            if campaign_manifest.is_file():
                history = json.loads(campaign_manifest.read_text(encoding="utf-8")).get("pairs", [])
                history = [entry for entry in history if entry.get("pair") != stem]
            history.append({"pair": stem, "marker": str(marker), "completed_at": completed,
                            "arms": len(records), "rows": sum(int(record["rows"]) for record in records)})
            atomic_json(campaign_manifest, {"version": 1, "mode": "data-only",
                                            "updated_at": now(), "pairs": history})
            print(f"completed {stem}: {sum(record['rows'] for record in records)} data rows")
            continue
        provenance: dict[tuple[str, str], dict[str, object]] = {}
        for scene in SCENES:
            resolve_scene = roots["pre"] / "scale_2_00" / scene
            raw = source_raw(resolve_scene)
            # Conventional controls and bilinear+CAS are actual transforms of
            # the matched decoded input frame, never copies of FSR output.
            control_outputs = {
                method: harness / "images" / filename(
                    scene, input_height, method, output_height)
                for method in ("conventional_lanczos", "conventional_bicubic",
                               "base_only_bilinear_cas20")
            }
            next(iter(control_outputs.values())).parent.mkdir(parents=True, exist_ok=True)
            final_w, final_h = (int(value) for value in final.split("x"))
            filters = (
                f"[0:v]split=3[l0][b0][c0];"
                f"[l0]scale={final_w}:{final_h}:flags=lanczos[l];"
                f"[b0]scale={final_w}:{final_h}:flags=bicubic[b];"
                f"[c0]scale={final_w}:{final_h}:flags=bilinear,cas=strength=0.20[c]"
            )
            runner.run([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(raw),
                "-filter_complex", filters,
                "-map", "[l]", "-frames:v", "1", str(control_outputs["conventional_lanczos"]),
                "-map", "[b]", "-frames:v", "1", str(control_outputs["conventional_bicubic"]),
                "-map", "[c]", "-frames:v", "1", str(control_outputs["base_only_bilinear_cas20"]),
            ], f"controls {scene} {input_height}->{output_height}")
            # At 2.00x the delivery grid equals the final image. These are the
            # direct/current controls, recorded as an explicit same-pipeline
            # provenance rather than silently pretending they were separate.
            direct = roots["pre"] / "scale_2_00" / scene / "candidate_final.png"
            export(runner, direct, scene, input_height, "fsr_direct_cas20", output_height, harness)
            export(runner, direct, scene, input_height, "current_cas20", output_height, harness)
            direct_record = experiment_provenance(resolve_scene, category="baseline")
            provenance[(scene, "fsr_direct_cas20")] = direct_record
            provenance[(scene, "current_cas20")] = dict(
                direct_record, alias_of=direct_record.get("experiment_id"))
            for placement, suffix in CAS_METHOD_SUFFIX.items():
                root = roots[placement]
                for scale in SCALES:
                    scene_root = root / f"scale_{scale:.2f}".replace(".", "_") / scene
                    export(runner, scene_root / "candidate_final.png",
                           scene, input_height, f"fsr_{int(scale * 100):03d}x_downsample_{suffix}",
                           output_height, harness)
                    export_native(runner, native_output(scene_root), scene, input_height,
                                  f"fsr_{int(scale * 100):03d}x_downsample_{suffix}",
                                  output_height, harness)
                    method = f"fsr_{int(scale * 100):03d}x_downsample_{suffix}"
                    provenance[(scene, method)] = experiment_provenance(
                        scene_root, category="baseline" if placement == "pre" else "diagnostic")
            for placement, suffix in CAS_METHOD_SUFFIX.items():
                native_scene_root = native_roots[placement] / "scale_2_00" / scene
                export(runner, native_scene_root / "candidate_final.png",
                       scene, input_height, f"fsr_nativeaa_downsample_{suffix}", output_height, harness)
                export_native(runner, native_output(native_scene_root), scene, input_height,
                              f"fsr_nativeaa_downsample_{suffix}", output_height, harness)
                method = f"fsr_nativeaa_downsample_{suffix}"
                provenance[(scene, method)] = experiment_provenance(
                    native_scene_root, category="nativeaa")
        missing = []
        for scene in SCENES:
            for method in METHODS:
                path = harness / "images" / filename(scene, input_height, method, output_height)
                if not path.is_file():
                    missing.append(str(path))
                if has_native_output(method) and not (harness / "images" / native_filename(
                        scene, input_height, method, output_height)).is_file():
                    missing.append(str(harness / "images" / native_filename(
                        scene, input_height, method, output_height)))
        if missing:
            raise SystemExit(f"{stem} incomplete: {len(missing)} harness assets missing")
        asset_records = [catalog_record(
            harness / "images" / filename(scene, input_height, method, output_height),
            scene=scene, method=method, input_height=input_height,
            output_height=output_height, view="delivery",
            provenance=provenance.get((scene, method), {
                "pipeline_category": "external_control" if method != "current_cas20" else "baseline_alias"
            }))
                         for scene in SCENES for method in METHODS]
        native_records = [catalog_record(
            harness / "images" / native_filename(scene, input_height, method, output_height),
            scene=scene, method=method, input_height=input_height,
            output_height=output_height, view="native",
            provenance=provenance.get((scene, method)))
                          for scene in SCENES for method in METHODS if has_native_output(method)]
        completed = now()
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker_data = {"input": input_height, "output": output_height,
                                      "scenes": list(SCENES), "methods": list(METHODS),
                                      "schemaVersion": 3, "runnerVersion": 3,
                                      "capture_plan": str(CAPTURE_PLAN_PATH),
                                      "campaign_id": CAPTURE_PLAN["campaignId"],
                                      "assets": len(asset_records), "native_assets": len(native_records),
                                      "started_at": pair_started, "completed_at": completed,
                                      "guard_log": str(runner.log), "assets_detail": asset_records,
                                      "native_assets_detail": native_records,
                                      "aliases": {
                                          "current_cas20": {"canonical_method": "fsr_direct_cas20", "reason": "catalog entries point to the direct arm's experiment ID per scene"},
                                      },
                                      "provenance": {
                                          "current_cas20": "same 2.00x resolve-CAS render as fsr_direct_cas20 at the delivery grid",
                                          "fsr_direct_cas20": "same 2.00x resolve-CAS render as current_cas20 at the delivery grid",
                                          "base_only_bilinear_cas20": "matched decoded input frame scaled with bilinear then CAS 0.20",
                                          "conventional_lanczos": "matched decoded input frame scaled with Lanczos",
                                          "conventional_bicubic": "matched decoded input frame scaled with bicubic",
                                      }}
        atomic_json(marker, marker_data)
        write_catalog(harness, asset_records + native_records)
        history = []
        if campaign_manifest.is_file():
            history = json.loads(campaign_manifest.read_text(encoding="utf-8")).get("pairs", [])
            history = [entry for entry in history if entry.get("pair") != stem]
        history.append({"pair": stem, "marker": str(marker), "completed_at": completed, "assets": len(asset_records)})
        atomic_json(campaign_manifest, {"version": 1, "updated_at": now(), "pairs": history})
        print(
            f"[{now()}] ROUTE {pair_index}/{len(selected_pairs)} DONE "
            f"{input_height}p -> {output_height}p: "
            f"{len(SCENES) * len(METHODS)} assets",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
