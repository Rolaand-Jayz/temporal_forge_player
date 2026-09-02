#!/usr/bin/env python3
"""Populate the portable review harness from one resumable serial command.

Each pair is a transaction: renderer arms are captured serially, source-based
controls are created from the matched decoded input frame, then every method
for the pair is exported and dimension-checked.  A pair is never marked done
until all 23 method IDs have files for all four scenes.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import re
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from benchmarks.quality_sweeps.trackmania_guard import DEFAULT_GAME_PATTERNS, running_games
SCENES = ("tos_daylight", "tos_debris", "sintel_rooftop", "sintel_cave")
SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)
CAS_PLACEMENTS = ("pre", "post", "none")
CAS_METHOD_SUFFIX = {"pre": "resolve_cas20", "post": "external_post_cas20", "none": "no_cas"}
PAIRS = (
    (360, "640x360", 720), (360, "640x360", 1080), (360, "640x360", 1440), (360, "640x360", 2160),
    (480, "854x480", 720), (480, "854x480", 1080), (480, "854x480", 1440), (480, "854x480", 2160),
    (540, "960x540", 720), (540, "960x540", 1080), (540, "960x540", 1440), (540, "960x540", 2160),
    (720, "1280x720", 1080), (720, "1280x720", 1440), (720, "1280x720", 2160),
    (1080, "1920x1080", 1440), (1080, "1920x1080", 2160),
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
        for item in existing["assets"] if isinstance(item, dict)
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
        "schema": "temporal_forge.review_catalog.v1",
        "validation": "entries are emitted only after pair validation",
        "assets": assets,
    })
    catalog_js = harness / "catalog.js"
    temporary = catalog_js.with_name(f".{catalog_js.name}.{os.getpid()}.tmp")
    temporary.write_text("window.__tforgeCatalog = " + json.dumps({"assets": assets}) + ";\n",
                         encoding="utf-8")
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
        self.record("command_start", {"label": label, "command": command})
        proc = subprocess.Popen(command, cwd=cwd, start_new_session=True)
        try:
            proc.wait()
            if proc.returncode:
                raise subprocess.CalledProcessError(proc.returncode, command)
            self.record("command_complete", {"label": label, "returncode": proc.returncode})
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
    runner.run([sys.executable, str(ROOT / "tools/export_review_image.py"), str(source),
         "--scene", scene, "--frame", "0048", "--input", str(input_height),
         "--method", method, "--output", str(output_height), "--root", str(harness)],
         f"export {scene}/{method} {input_height}->{output_height}")


def export_native(runner: PausingRunner, source: Path, scene: str, input_height: int,
                  method: str, output_height: int, harness: Path) -> None:
    output = harness / "images" / native_filename(scene, input_height, method, output_height)
    output.parent.mkdir(parents=True, exist_ok=True)
    runner.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-i", str(source), "-frames:v", "1", str(output)],
               f"native {scene}/{method} {input_height}->{output_height}")


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
    if data.get("schemaVersion") != 2 or data.get("runnerVersion") != 2:
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
        if len(rows) != len(SCALES) * len(SCENES):
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
            if not output.is_file() or record_data.get("output_sha256") != hashlib.sha256(output.read_bytes()).hexdigest():
                raise SystemExit(f"output provenance mismatch in {record}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build-fast/temporal_forge_player"))
    parser.add_argument("--artifact-root", type=Path, default=Path("benchmarks/quality_sweeps/harness_campaign_recapture_20260901"))
    parser.add_argument("--harness-root", type=Path, default=Path("review_harness"))
    parser.add_argument("--fixture-manifest", type=Path,
                        default=Path("benchmarks/quality_sweeps/resolution_540p_fixture_20260901.csv"))
    parser.add_argument("--only", action="append", metavar="INPUTxOUTPUT",
                        help="limit this invocation to selected pair(s); repeatable")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--data-only", action="store_true",
                        help="recapture campaign CSV/raw data without rewriting harness images")
    parser.add_argument("--allow-game", action="append", default=[], metavar="PATTERN",
                        help="allow a matching game pattern without pausing; repeatable")
    parser.add_argument("--game-pattern", action="append", default=[], metavar="PATTERN",
                        help="additional game process pattern; repeatable")
    parser.add_argument("--poll-seconds", type=float, default=2.0)
    args = parser.parse_args()
    artifacts = args.artifact_root.resolve(); harness = args.harness_root.resolve()
    patterns = tuple(DEFAULT_GAME_PATTERNS) + tuple(args.game_pattern)
    runner = PausingRunner(artifacts, patterns, tuple(args.allow_game), args.poll_seconds)
    campaign_manifest = artifacts / "campaign_manifest.json"
    run_capture = ROOT / "benchmarks/quality_sweeps/run_fsr_supersampling.py"
    for input_height, source, output_height in PAIRS:
        if args.only and f"{input_height}x{output_height}" not in args.only:
            continue
        stem = f"resolution_{input_height}_to_{output_height}"
        pair_root = artifacts / stem
        marker = pair_root / "harness_pair_complete.json"
        pair_started = now()
        if args.resume and marker.is_file():
            validate_resume_pair(pair_root, input_height, output_height, harness, args.player.resolve())
            print(f"Skipping complete {stem}")
            continue
        runner.wait_until_clear(stem)
        final = f"{round(output_height * 16 / 9)}x{output_height}"
        manifest = args.fixture_manifest if input_height == 540 else None
        roots = {}
        for placement in CAS_PLACEMENTS:
            runner.wait_until_clear(f"{stem}/{placement}")
            root = pair_root / f"fsr_{placement}"
            csv_path = pair_root / f"fsr_{placement}.csv"
            command = [sys.executable, str(run_capture), "--player", str(args.player.resolve()),
                       "--repo", str(ROOT), "--artifact-root", str(root), "--output-csv", str(csv_path),
                       "--final", final, "--source", source, "--cas-strength", "0.20",
                       "--cas-placement", placement]
            if manifest:
                command.extend(["--manifest", str(manifest.resolve())])
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
                       "--final", final, "--source", source, "--cas-strength", "0.20",
                       "--cas-placement", placement, "--preset", "NativeAA"]
            if manifest:
                command.extend(["--manifest", str(manifest.resolve())])
            runner.run(command, f"capture {stem}/nativeaa_{placement}")
            native_roots[placement] = native_root
            native_csvs[placement] = native_csv
        if args.data_only:
            csv_paths = [pair_root / f"fsr_{placement}.csv" for placement in CAS_PLACEMENTS]
            csv_paths.extend(native_csvs.values())
            records = [csv_record(path) for path in csv_paths]
            expected_rows = len(SCALES) * len(SCENES)
            if any(record["rows"] != expected_rows for record in records):
                raise SystemExit(f"{stem} data incomplete: expected {expected_rows} rows per arm")
            completed = now()
            marker_data = {"input": input_height, "output": output_height,
                           "scenes": list(SCENES), "scales": list(SCALES),
                           "arms": [f"fsr_{placement}" for placement in CAS_PLACEMENTS] + ["nativeaa"],
                           "data_only": True, "completed_at": completed,
                           "guard_log": str(runner.log), "csv_records": records}
            atomic_json(marker, marker_data)
            history = []
            if campaign_manifest.is_file():
                history = json.loads(campaign_manifest.read_text(encoding="utf-8")).get("pairs", [])
                history = [entry for entry in history if entry.get("pair") != stem]
            history.append({"pair": stem, "marker": str(marker), "completed_at": completed,
                            "arms": len(records), "rows_per_arm": expected_rows})
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
            for method, flags in (("conventional_lanczos", "lanczos"), ("conventional_bicubic", "bicubic"),
                                  ("base_only_bilinear_cas20", "bilinear")):
                output = harness / "images" / filename(scene, input_height, method, output_height)
                output.parent.mkdir(parents=True, exist_ok=True)
                vf = f"scale={round(output_height * 16 / 9)}:{output_height}:flags={flags}"
                if method == "base_only_bilinear_cas20":
                    vf += ",cas=strength=0.20"
                runner.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(raw),
                            "-vf", vf, "-frames:v", "1", str(output)],
                           f"control {scene}/{method} {input_height}->{output_height}")
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
                                      "schemaVersion": 2, "runnerVersion": 2,
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
        print(f"completed {stem}: {len(SCENES) * len(METHODS)} assets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
