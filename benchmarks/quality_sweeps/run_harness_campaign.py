#!/usr/bin/env python3
"""Populate the portable review harness from one resumable serial command.

Each pair is a transaction: renderer arms are captured serially, source-based
controls are created from the matched decoded input frame, then every method
for the pair is exported and dimension-checked.  A pair is never marked done
until all 23 method IDs have files for all three scenes.
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
SCENES = ("tos_daylight", "sintel_rooftop", "sintel_cave")
SCALES = (2.00, 2.25, 2.50, 2.75, 3.00)
PAIRS = (
    (360, "640x360", 720), (360, "640x360", 1080), (360, "640x360", 1440), (360, "640x360", 2160),
    (480, "854x480", 720), (480, "854x480", 1080), (480, "854x480", 1440), (480, "854x480", 2160),
    (540, "960x540", 720), (540, "960x540", 1080), (540, "960x540", 1440), (540, "960x540", 2160),
    (720, "1280x720", 1080), (720, "1280x720", 1440), (720, "1280x720", 2160),
    (1080, "1920x1080", 1440), (1080, "1920x1080", 2160),
)
METHODS = (
    "current_cas20", "base_only_bilinear_cas20", "fsr_direct_cas20",
    *(f"fsr_{int(scale * 100):03d}x_downsample_{placement}"
      for scale in SCALES for placement in ("cas20_pre", "cas20_post", "no_cas")),
    "fsr_nativeaa_downsample_cas20_pre", "fsr_nativeaa_downsample_cas20_post",
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


def csv_record(path: Path) -> dict[str, object]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    return {"path": str(path), "bytes": path.stat().st_size, "rows": len(rows),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "mtime": dt.datetime.fromtimestamp(path.stat().st_mtime,
                                                 dt.timezone.utc).isoformat()}


class PausingRunner:
    def __init__(self, artifact_root: Path, patterns: tuple[str, ...], allow_games: tuple[str, ...], poll_seconds: float = 2.0) -> None:
        self.patterns = patterns
        self.allow_games = allow_games
        self.poll_seconds = poll_seconds
        self.log = artifact_root / "game_guard_events.jsonl"
        self.log.parent.mkdir(parents=True, exist_ok=True)

    def games(self) -> list[dict[str, str]]:
        return running_games(self.patterns, self.allow_games)

    def record(self, event: str, details: object) -> None:
        with self.log.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps({"timestamp": now(), "event": event, "games": details}) + "\n")

    def wait_until_clear(self, label: str) -> None:
        announced = False
        while True:
            games = self.games()
            if not games:
                if announced:
                    self.record("resume_before_command", {"label": label})
                return
            if not announced:
                self.record("pause_before_command", {"label": label, "processes": games})
                print(f"paused before {label}: game process detected; waiting", flush=True)
                announced = True
            time.sleep(self.poll_seconds)

    def run(self, command: list[str], label: str, cwd: Path = ROOT) -> None:
        self.wait_until_clear(label)
        self.record("command_start", {"label": label, "command": command})
        proc = subprocess.Popen(command, cwd=cwd, start_new_session=True)
        paused = False
        try:
            while proc.poll() is None:
                games = self.games()
                if games and not paused:
                    os.killpg(proc.pid, signal.SIGSTOP)
                    paused = True
                    self.record("pause_running_command", {"label": label, "processes": games})
                    print(f"paused {label}: game process detected; waiting", flush=True)
                elif not games and paused:
                    os.killpg(proc.pid, signal.SIGCONT)
                    paused = False
                    self.record("resume_running_command", {"label": label})
                    print(f"resumed {label}", flush=True)
                time.sleep(self.poll_seconds)
            if paused:
                os.killpg(proc.pid, signal.SIGCONT)
            if proc.returncode:
                raise subprocess.CalledProcessError(proc.returncode, command)
            self.record("command_complete", {"label": label, "returncode": proc.returncode})
        except BaseException:
            if proc.poll() is None:
                if paused:
                    os.killpg(proc.pid, signal.SIGCONT)
                proc.terminate()
                proc.wait()
            raise


def filename(scene: str, input_height: int, method: str, output_height: int) -> str:
    return (f"scene-{scene}__frame-0048__in-{input_height}p__method-{method}"
            f"__out-{output_height}p.png")


def export(runner: PausingRunner, source: Path, scene: str, input_height: int, method: str, output_height: int, harness: Path) -> None:
    runner.run([sys.executable, str(ROOT / "tools/export_review_image.py"), str(source),
         "--scene", scene, "--frame", "0048", "--input", str(input_height),
         "--method", method, "--output", str(output_height), "--root", str(harness)],
         f"export {scene}/{method} {input_height}->{output_height}")


def source_raw(scene_root: Path) -> Path:
    candidates = sorted((scene_root / "quality_frames").glob("*_gpu_raw.png"))
    if len(candidates) != 1:
        raise SystemExit(f"expected one matched decoded source frame in {scene_root}, got {candidates}")
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, default=Path("build/temporal_forge_player"))
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
            print(f"Skipping complete {stem}")
            continue
        runner.wait_until_clear(stem)
        final = f"{round(output_height * 16 / 9)}x{output_height}"
        manifest = args.fixture_manifest if input_height == 540 else None
        roots = {}
        for placement in ("pre", "post", "none"):
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
        native_root = pair_root / "nativeaa"
        native_csv = pair_root / "nativeaa.csv"
        runner.wait_until_clear(f"{stem}/nativeaa")
        command = [sys.executable, str(run_capture), "--player", str(args.player.resolve()),
                   "--repo", str(ROOT), "--artifact-root", str(native_root), "--output-csv", str(native_csv),
                   "--final", final, "--source", source, "--cas-strength", "0.20",
                   "--preset", "NativeAA"]
        if manifest:
            command.extend(["--manifest", str(manifest.resolve())])
        runner.run(command, f"capture {stem}/nativeaa")
        if args.data_only:
            csv_paths = [pair_root / f"fsr_{placement}.csv" for placement in ("pre", "post", "none")]
            csv_paths.append(native_csv)
            records = [csv_record(path) for path in csv_paths]
            expected_rows = len(SCALES) * len(SCENES)
            if any(record["rows"] != expected_rows for record in records):
                raise SystemExit(f"{stem} data incomplete: expected {expected_rows} rows per arm")
            completed = now()
            marker_data = {"input": input_height, "output": output_height,
                           "scenes": list(SCENES), "scales": list(SCALES),
                           "arms": ["fsr_pre", "fsr_post", "fsr_none", "nativeaa"],
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
        for scene in SCENES:
            pre_scene = roots["pre"] / "scale_2_00" / scene
            raw = source_raw(pre_scene)
            # Conventional controls and bilinear+CAS are actual transforms of
            # the matched decoded input frame, never copies of FSR output.
            for method, flags in (("conventional_lanczos", "lanczos"), ("conventional_bicubic", "bicubic"),
                                  ("base_only_bilinear_cas20", "bilinear")):
                output = harness / "images" / filename(scene, input_height, method, output_height)
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
            for placement, suffix in (("pre", "cas20_pre"), ("post", "cas20_post"), ("none", "no_cas")):
                root = roots[placement]
                for scale in SCALES:
                    export(runner, root / f"scale_{scale:.2f}".replace(".", "_") / scene / "candidate_final.png",
                           scene, input_height, f"fsr_{int(scale * 100):03d}x_downsample_{suffix}",
                           output_height, harness)
            for placement, suffix in (("pre", "cas20_pre"), ("post", "cas20_post"), ("none", "no_cas")):
                export(runner, native_root / "scale_2_00" / scene / "candidate_final.png",
                       scene, input_height, f"fsr_nativeaa_downsample_{suffix}", output_height, harness)
        missing = []
        for scene in SCENES:
            for method in METHODS:
                path = harness / "images" / filename(scene, input_height, method, output_height)
                if not path.is_file():
                    missing.append(str(path))
        if missing:
            raise SystemExit(f"{stem} incomplete: {len(missing)} harness assets missing")
        asset_records = [image_record(harness / "images" / filename(scene, input_height, method, output_height))
                         for scene in SCENES for method in METHODS]
        completed = now()
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker_data = {"input": input_height, "output": output_height,
                                      "scenes": list(SCENES), "methods": list(METHODS),
                                      "assets": len(SCENES) * len(METHODS),
                                      "started_at": pair_started, "completed_at": completed,
                                      "guard_log": str(runner.log), "assets_detail": asset_records,
                                      "provenance": {
                                          "current_cas20": "same 2.00x pre-CAS render as fsr_direct_cas20 at the delivery grid",
                                          "fsr_direct_cas20": "same 2.00x pre-CAS render as current_cas20 at the delivery grid",
                                          "base_only_bilinear_cas20": "matched decoded input frame scaled with bilinear then CAS 0.20",
                                          "conventional_lanczos": "matched decoded input frame scaled with Lanczos",
                                          "conventional_bicubic": "matched decoded input frame scaled with bicubic",
                                      }}
        atomic_json(marker, marker_data)
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
