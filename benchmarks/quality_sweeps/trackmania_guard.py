"""Personal-device guardrails for sample-producing quality runs."""

from __future__ import annotations

import os
from pathlib import Path


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


def guarded_worker_count(requested: int) -> tuple[int, bool]:
    """Force sequential sample collection while Trackmania is running."""
    active = trackmania_is_running()
    return (1 if active else requested), active
