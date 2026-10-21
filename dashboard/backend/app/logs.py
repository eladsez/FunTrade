"""Warnings and errors from the bot's log (spdlog: `[date time.ms] [name] [level] message`),
grouped by bot run (the journal's start events) with repeats folded, so a problem fixed by a
restart doesn't look current and a warning repeated at every start shows once with a count."""
import bisect
import re
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)(?:\.(\d+))?\] \[[^\]]*\] \[(warning|error|critical)\] (.*)$")
TAIL_BYTES = 2 * 1024 * 1024
NY = ZoneInfo("America/New_York")   # the bot's container runs on New York time


def _epoch_ms(stamp: str, frac: str | None) -> int:
    t = datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S").replace(tzinfo=NY)
    return int(t.timestamp() * 1000) + int((frac or "0")[:3].ljust(3, "0"))


def _tail(log_file: Path) -> str:
    with log_file.open("rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - TAIL_BYTES))
        return f.read().decode("utf-8", "replace")


def problems(log_file: Path, starts: list[int] | None = None, limit: int = 100) -> list[dict]:
    """Runs newest first: {started_at (epoch ms; None = before the first journaled start),
    current, lines}. Lines are distinct messages, newest first, each with how often it was
    logged in that run and when first/last (times as logged, New York)."""
    try:
        text = _tail(log_file)
    except FileNotFoundError:
        return []
    starts = sorted(starts or [])
    runs: dict[int | None, dict[tuple, dict]] = {}
    for raw in text.splitlines():
        m = LINE.match(raw)
        if not m:
            continue
        stamp, frac, level, message = m.groups()
        t = _epoch_ms(stamp, frac)
        i = bisect.bisect_right(starts, t)
        run = starts[i - 1] if i else None
        level = "error" if level == "critical" else level
        lines = runs.setdefault(run, {})
        seen = lines.get((level, message))
        if seen:
            seen.update(count=seen["count"] + 1, last=stamp, t=t)
        else:
            lines[(level, message)] = {"time": stamp, "last": stamp, "t": t, "level": level, "message": message, "count": 1}
    current = starts[-1] if starts else None
    out = []
    for run in sorted(runs, key=lambda r: -1 if r is None else r, reverse=True):
        lines = sorted(runs[run].values(), key=lambda x: x["t"], reverse=True)[:limit]
        out.append({"started_at": run, "current": run == current,
                    "lines": [{k: v for k, v in x.items() if k != "t"} for x in lines]})
    return out
