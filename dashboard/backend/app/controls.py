"""The only writes the dashboard makes to the bot's state/: the KILL and FLATTEN control
files the bot already reads, and clearing a halted basket while the bot is stopped (the
runbook procedure, with a backup first). Every action goes to the dashboard's audit log."""
import json
import os
import shutil
import time
from pathlib import Path

from . import store, views

FLAGS = {"kill": "KILL", "flatten": "FLATTEN"}


class ControlError(Exception):
    pass


def audit(data_dir: Path, action: str, who: dict, **details):
    data_dir.mkdir(parents=True, exist_ok=True)
    line = {"t": int(time.time() * 1000), "action": action, **who, **details}
    with (data_dir / "audit.jsonl").open("a") as f:
        f.write(json.dumps(line) + "\n")


def set_flag(state_dir: Path, data_dir: Path, flag: str, on: bool, confirm: str, who: dict) -> bool:
    name = FLAGS.get(flag)
    if not name:
        raise ControlError(f"unknown control {flag}")
    if confirm != name:
        raise ControlError(f"type {name} to confirm")
    path = state_dir / name
    if on:
        path.write_text(f"set from the dashboard at {time.strftime('%Y-%m-%d %H:%M:%S %Z')}\n")
    else:
        path.unlink(missing_ok=True)
    audit(data_dir, f"{flag}_{'on' if on else 'off'}", who)
    return path.exists()


def _write_json_atomic(path: Path, data):
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w") as f:
        json.dump(data, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def clear_halt(state_dir: Path, data_dir: Path, basket: str, confirm: str, remove_books: bool, who: dict) -> dict:
    """OPERATIONS.md 'Clearing a halt', step 3: delete the basket's state file and, when its
    legs were closed (or never existed) at the broker, its book entries. Only while the bot
    is stopped: a running bot would rewrite both from memory."""
    if confirm != basket:
        raise ControlError("type the basket id to confirm")
    if not views.bot_stopped(store.read_status(state_dir)):
        raise ControlError("the bot is running; stop it first (docker compose stop funtrade)")
    bpath = state_dir / "baskets" / f"{basket}.json"
    st = store.read_json(bpath)
    if not st or st.get("master_state") != "SYSTEM_HALTED":
        raise ControlError(f"{basket} is not halted")

    backup = data_dir / "backups" / f"{time.strftime('%Y%m%d-%H%M%S')}-{basket}"
    backup.mkdir(parents=True)
    shutil.copy2(bpath, backup / bpath.name)
    removed = {}
    ppath = state_dir / "positions.json"
    if remove_books and ppath.exists():
        shutil.copy2(ppath, backup / "positions.json")
        pj = json.loads(ppath.read_text())
        removed = {k: v for k, v in pj.get("positions", {}).items() if store.basket_of(k) == basket}
        for k in removed:
            del pj["positions"][k]
        _write_json_atomic(ppath, pj)
    bpath.unlink()
    audit(data_dir, "clear_halt", who, basket=basket, reason=st.get("halt_reason"),
          removed_keys=sorted(removed), backup=str(backup))
    return {"removed_keys": sorted(removed), "backup": str(backup)}
