"""Reads what the bot writes: basket configs, state/baskets/*.json, state/positions.json,
state/status.json and the append-only state/journal.jsonl. Nothing here writes."""
import json
import threading
from collections import defaultdict
from dataclasses import dataclass, field
from datetime import date, timedelta
from pathlib import Path

EPOCH = date(1970, 1, 1)


def day_iso(day: int | None) -> str | None:
    """The bot's trading day (days since the epoch, = the bar's calendar date) as YYYY-MM-DD."""
    return (EPOCH + timedelta(days=int(day))).isoformat() if day else None


def read_json(path: Path):
    """None if missing. A torn or corrupt file reads as None too: the bot writes atomically,
    so that only happens mid-replace on non-POSIX filesystems; the next poll gets it."""
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def basket_of(position_key: str) -> str:
    return position_key.rpartition("_")[0]


def load_configs(config_dir: Path) -> dict[str, dict]:
    out = {}
    for p in sorted((config_dir / "basket_configs").glob("*.json")):
        c = read_json(p)
        if not c:
            continue
        tp = c.get("trading_params", {})
        out[c["basket_id"]] = {
            "id": c["basket_id"],
            "tickers": c["tickers"],
            "weights": c["weights"],
            "window": tp.get("window_size"),
            "entry_z": tp.get("entry_z"),
            "exit_z": tp.get("exit_z"),
            "metrics": c.get("metrics", {}),
            "exported_at": c.get("exported_at"),
            "periods": c.get("backtest_periods", {}),
        }
    return out


def load_risk(config_dir: Path) -> dict:
    return read_json(config_dir / "risk.json") or {}


def read_baskets(state_dir: Path) -> dict[str, dict]:
    out = {}
    for p in sorted((state_dir / "baskets").glob("*.json")):
        b = read_json(p)
        if b and "basket_id" in b:
            out[b["basket_id"]] = b
    return out


def read_positions(state_dir: Path) -> dict[str, dict]:
    j = read_json(state_dir / "positions.json") or {}
    return j.get("positions", {})


def read_status(state_dir: Path) -> dict | None:
    return read_json(state_dir / "status.json")


_zcache: dict[Path, tuple[int, list]] = {}


def read_zhistory(state_dir: Path, basket: str) -> list[tuple[int, float]]:
    """state/zhistory/<id>.json: the bot's z for each day of its warm-up history, written at
    startup. [(day, z)], oldest first; [] if missing. Cached by mtime (61 files per page)."""
    p = state_dir / "zhistory" / f"{basket}.json"
    try:
        mtime = p.stat().st_mtime_ns
    except FileNotFoundError:
        return []
    hit = _zcache.get(p)
    if hit and hit[0] == mtime:
        return hit[1]
    days = [(int(d), float(z)) for d, z in (read_json(p) or {}).get("days", [])]
    _zcache[p] = (mtime, days)
    return days


def control_flags(state_dir: Path) -> dict[str, bool]:
    return {"kill": (state_dir / "KILL").exists(), "flatten": (state_dir / "FLATTEN").exists()}


@dataclass
class JournalIndex:
    """Incremental reader of state/journal.jsonl, indexed per basket. refresh() reads only
    the bytes appended since the last call (the file is append-only); a shorter file means
    it was replaced, so it starts over."""
    path: Path
    _offset: int = 0
    _partial: bytes = b""
    closes: dict = field(default_factory=lambda: defaultdict(list))     # basket -> [{day, z, action, state, position, t}]
    cycles: dict = field(default_factory=lambda: defaultdict(list))     # basket -> [{start, end, fills}]
    fills: list = field(default_factory=list)                           # every fill, oldest first
    skipped: dict = field(default_factory=lambda: defaultdict(list))    # basket -> [entry_skipped]
    halts: dict = field(default_factory=lambda: defaultdict(list))
    runs: list = field(default_factory=list)                            # start/stop events
    _lock: threading.Lock = field(default_factory=threading.Lock)

    def refresh(self) -> bool:
        """True if anything new was read."""
        with self._lock:
            try:
                size = self.path.stat().st_size
            except FileNotFoundError:
                return False
            if size < self._offset:
                self._reset()
            if size == self._offset:
                return False
            with self.path.open("rb") as f:
                f.seek(self._offset)
                chunk = self._partial + f.read(size - self._offset)
            self._offset = size
            lines = chunk.split(b"\n")
            self._partial = lines.pop()  # incomplete last line (a write in progress)
            for line in lines:
                if line.strip():
                    try:
                        self._apply(json.loads(line))
                    except (json.JSONDecodeError, KeyError, TypeError):
                        continue
            return True

    def _reset(self):
        self._offset = 0
        self._partial = b""
        self.closes.clear()
        self.cycles.clear()
        self.fills.clear()
        self.skipped.clear()
        self.halts.clear()
        self.runs.clear()

    def _apply(self, e: dict):
        kind = e["type"]
        b = e.get("basket")
        if kind == "close":
            closes = self.closes[b]
            # A restart can re-record a day already decided; the latest record wins.
            if closes and closes[-1]["day"] == e["day"]:
                closes.pop()
            closes.append({k: e.get(k) for k in ("t", "day", "z", "action", "state", "position")})
        elif kind == "cycle_start":
            self.cycles[b].append({"start": e, "end": None, "fills": []})
        elif kind == "cycle_end":
            open_cycles = [c for c in self.cycles[b] if c["end"] is None]
            if open_cycles:
                open_cycles[-1]["end"] = e
        elif kind == "fill":
            self.fills.append(e)
            for c in reversed(self.cycles.get(b, [])):
                ids = {leg.get("client_order_id") for leg in c["start"].get("legs", [])}
                if e.get("client_order_id") in ids:
                    c["fills"].append(e)
                    break
        elif kind == "entry_skipped":
            self.skipped[b].append(e)
        elif kind == "halt":
            self.halts[b].append(e)
        elif kind in ("start", "stop"):
            self.runs.append(e)

    def snapshot_for(self, basket: str) -> dict:
        with self._lock:
            return {
                "closes": list(self.closes.get(basket, [])),
                "cycles": [dict(c, fills=list(c["fills"])) for c in self.cycles.get(basket, [])],
                "skipped": list(self.skipped.get(basket, [])),
                "halts": list(self.halts.get(basket, [])),
            }

    def last_close(self, basket: str) -> dict | None:
        with self._lock:
            c = self.closes.get(basket)
            return c[-1] if c else None

    def open_cycle(self, basket: str) -> dict | None:
        with self._lock:
            cs = self.cycles.get(basket)
            return cs[-1] if cs and cs[-1]["end"] is None else None

    def last_entry(self, basket: str) -> dict | None:
        """The ENTRY cycle that opened the current position, if journaled."""
        with self._lock:
            for c in reversed(self.cycles.get(basket, [])):
                if c["start"].get("label") == "ENTRY":
                    return c
            return None

    def recent_fills(self, n: int) -> list:
        with self._lock:
            return self.fills[-n:][::-1]

    def recent_events(self, since_ms: int) -> list:
        """Everything after since_ms, oldest first, for the 'today' timeline."""
        with self._lock:
            out = []
            for b, cs in self.cycles.items():
                for c in cs:
                    if c["start"]["t"] >= since_ms:
                        out.append(c["start"])
                    if c["end"] and c["end"]["t"] >= since_ms:
                        out.append(c["end"])
            for src in (self.skipped, self.halts):
                for b, es in src.items():
                    out.extend(e for e in es if e["t"] >= since_ms)
            out.extend(e for e in self.runs if e["t"] >= since_ms)
            return sorted(out, key=lambda e: e["t"])

    def last_run(self) -> dict | None:
        with self._lock:
            return self.runs[-1] if self.runs else None

    def latest_close_t(self) -> int | None:
        with self._lock:
            ts = [c[-1]["t"] for c in self.closes.values() if c]
            return max(ts) if ts else None
