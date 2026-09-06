"""Personal-device guardrails for sample-producing quality runs.

The capture runner uses this module as a pause gate. Detection is deliberately
process-based and configurable because Linux has no universal "is a game"
process flag.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Iterable


DEFAULT_GAME_PATTERNS = (
    "trackmania", "steam_app_", "gamescope", "lutris", "heroic", "proton",
    "wine", "minecraft", "factorio", "eldenring", "rocketleague", "dota2",
    "cs2", "apex", "fortnite", "valorant", "warframe", "baldursgate",
    "cyberpunk", "witcher", "skyrim", "fallout", "starfield", "overwatch",
    "leagueoflegends", "genshin", "destiny2", "civilization", "diablo", "wow",
)


def trackmania_is_running() -> bool:
    """Return whether a Trackmania game or launcher process is present."""
    proc_root = Path("/proc")
    own_pid = os.getpid()
    ancestor_pids: set[int] = set()
    parent_pid = os.getppid()
    while parent_pid > 1 and parent_pid not in ancestor_pids:
        ancestor_pids.add(parent_pid)
        try:
            parent_pid = int((proc_root / str(parent_pid) / "stat").read_text().split()[3])
        except (OSError, ValueError, IndexError):
            break
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid == own_pid or pid in ancestor_pids:
            continue
        try:
            comm = (entry / "comm").read_text(encoding="utf-8", errors="replace")
            argv = (entry / "cmdline").read_bytes().decode("utf-8", errors="replace").split("\x00")
        except OSError:
            continue
        executable = Path(argv[0]).name if argv and argv[0] else ""
        if "trackmania" in f"{comm} {executable}".casefold():
            return True
    return False


def running_games(patterns: Iterable[str] = DEFAULT_GAME_PATTERNS,
                  allow: Iterable[str] = ()) -> list[dict[str, str]]:
    """Return matching game processes, excluding explicitly allowed patterns."""
    wanted = tuple(re.compile(p.casefold()) for p in patterns if p)
    ignored = tuple(re.compile(p.casefold()) for p in allow if p)
    own_pid = os.getpid()
    parent_pid = os.getppid()
    excluded = {own_pid}
    while parent_pid > 1 and parent_pid not in excluded:
        excluded.add(parent_pid)
        try:
            parent_pid = int((Path('/proc') / str(parent_pid) / 'stat').read_text().split()[3])
        except (OSError, ValueError, IndexError):
            break
    matches: list[dict[str, str]] = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) in excluded:
            continue
        try:
            comm = (entry / 'comm').read_text(encoding='utf-8', errors='replace').strip()
            argv = (entry / 'cmdline').read_bytes().decode('utf-8', errors='replace').replace('\x00', ' ')
        except OSError:
            continue
        text = f'{comm} {argv}'.casefold()
        hit = next((p.pattern for p in wanted if p.search(text)), None)
        if hit and not any(p.search(text) for p in ignored):
            matches.append({'pid': entry.name, 'comm': comm, 'match': hit})
    return matches


def games_are_running(allow: Iterable[str] = ()) -> bool:
    return bool(running_games(allow=allow))


def guarded_worker_count(requested: int) -> tuple[int, bool]:
    """Force sequential sample collection while Trackmania is running."""
    active = trackmania_is_running()
    return (1 if active else requested), active
