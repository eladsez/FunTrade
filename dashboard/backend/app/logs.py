"""Warnings and errors from the bot's log (spdlog: `[date time.ms] [name] [level] message`)."""
import re
from pathlib import Path

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)(?:\.\d+)?\] \[[^\]]*\] \[(warning|error|critical)\] (.*)$")
TAIL_BYTES = 2 * 1024 * 1024


def problems(log_file: Path, limit: int = 100) -> list[dict]:
    """The latest `limit` warning/error lines, newest first. Times are as logged (the bot's
    container runs on New York time)."""
    try:
        with log_file.open("rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - TAIL_BYTES))
            text = f.read().decode("utf-8", "replace")
    except FileNotFoundError:
        return []
    out = []
    for raw in text.splitlines():
        m = LINE.match(raw)
        if m:
            out.append({"time": m.group(1), "level": "error" if m.group(2) == "critical" else m.group(2),
                        "message": m.group(3)})
    return out[-limit:][::-1]
