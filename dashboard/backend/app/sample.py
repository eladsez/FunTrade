"""A realistic, made-up state/ + logs/ for tests and local development (no Pi, no broker):
    python -m app.sample <state_dir> <logs_dir>
    python -m app.sample heartbeat <state_dir>    # keep status.json fresh, like a running bot
One basket in position, five entries working, one halted, the rest flat, ~90 days of
journaled closes. All numbers are invented."""
import json
import math
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

from . import store

NY = ZoneInfo("America/New_York")

PENDING = {
    "hban_schw_trv": ("LONG", {"HBAN": ("BUY", 241), "SCHW": ("SELL", 38), "TRV": ("SELL", 5)}, -2.14),
    "ajg_hban_schw": ("SHORT", {"AJG": ("SELL", 6), "HBAN": ("BUY", 118), "SCHW": ("SELL", 21)}, 2.31),
    "ball_ip_sw": ("SHORT", {"BALL": ("SELL", 29), "IP": ("BUY", 40), "SW": ("BUY", 33)}, 2.47),
}
IN_POS = ("fitb_key_tfc", {"FITB": (52, 44.18), "KEY": (-61, 17.92), "TFC": (18, 46.05)})
HALTED = ("cms_ed_ppl", "reconciliation mismatch at startup: ED books 12.0000, broker 0.0000")


def trading_days(n: int, end: int) -> list[int]:
    """The last n weekdays up to and including day `end` (epoch days)."""
    out, d = [], end
    while len(out) < n:
        if (store.EPOCH + timedelta(days=d)).weekday() < 5:
            out.append(d)
        d -= 1
    return out[::-1]


def z_path(i: int, seed: float) -> float:
    return 1.3 * math.sin(i / 7 + seed) + 0.5 * math.sin(i / 2.3 + 2 * seed) + 0.2 * math.cos(i * 1.7 + seed)


def leg_close(symbol: str, i: int) -> float:
    """A made-up daily close for a symbol on the i-th sample day."""
    held = {**{s: px for s, (_, px) in IN_POS[1].items()}, "ED": 101.4}.get(symbol)
    base = held or 20 + sum(map(ord, symbol)) % 180   # held legs trade near their fills
    return base * (1 + 0.02 * math.sin(i / 5 + len(symbol)))


def write(state: Path, logs: Path, config_dir: Path, now: float | None = None, data: Path | None = None):
    now = now or time.time()
    ny_now = datetime.fromtimestamp(now, NY)
    today = (ny_now.date() - store.EPOCH).days
    days = trading_days(90, today - 1)
    ms = lambda d, h=16: int((datetime.combine(store.EPOCH + timedelta(days=d), datetime.min.time(), NY)
                              + timedelta(hours=h)).timestamp() * 1000)
    (state / "baskets").mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    started = int(now * 1000) - 6 * 3600_000
    before = started - 22 * 3600_000   # the run before this one, stopped for a deploy
    journal = [{"t": before, "type": "start", "git_sha": "1f00ba7e", "baskets": 61},
               {"t": started - 60_000, "type": "stop"},
               {"t": started, "type": "start", "git_sha": "9efb6c1d", "baskets": 61}]

    # Every basket gets closes; the special ones end where their story says.
    configs = store.load_configs(config_dir)
    entry_day = days[-6]
    for k, bid in enumerate(sorted(configs) or [IN_POS[0]]):
        for i, d in enumerate(days):
            z = z_path(i, k * 0.37)
            action = "NONE"
            if bid == IN_POS[0]:
                if i >= len(days) - 6:
                    z = -2.11 + (i - (len(days) - 6)) * 0.24
                if d == days[-7]:
                    z, action = -2.11, "ENTER_LONG"
            if bid in PENDING and d == days[-1]:
                z = PENDING[bid][2]
                action = "ENTER_" + PENDING[bid][0]
            pos = "LONG" if bid == IN_POS[0] and d >= entry_day else "FLAT"
            closes = {sym: round(leg_close(sym, i), 2) for sym in configs.get(bid, {}).get("tickers", [])}
            if bid == IN_POS[0] and d == entry_day:
                closes = {sym: round(px - 0.03, 2) for sym, (_, px) in IN_POS[1].items()}  # the fills, vs the close
            journal.append({"t": ms(d, 18), "type": "close", "basket": bid, "day": d, "z": round(z, 4),
                            "state": "IN_POSITION" if pos == "LONG" else "FLAT", "position": pos, "action": action,
                            "closes": closes})

    # fitb_key_tfc: entered at the close of entry_day.
    bid, legs = IN_POS
    cids = {s: f"{bid}_{s}#{ms(entry_day, 9)}-{n}" for n, s in enumerate(legs)}
    journal.append({"t": ms(entry_day, 9) + 60_000, "type": "cycle_start", "basket": bid, "label": "ENTRY", "day": entry_day,
                    "position": "LONG", "legs": [{"symbol": s, "side": "BUY" if q > 0 else "SELL", "quantity": abs(q),
                                                  "client_order_id": cids[s]} for s, (q, _) in legs.items()]})
    seen = []
    for s, (q, px) in legs.items():
        eid = f"0000e0d5.{s}.01.01"
        seen.append(eid)
        journal.append({"t": ms(entry_day, 16) + 5000, "type": "fill", "basket": bid, "position_key": f"{bid}_{s}",
                        "symbol": s, "side": "BUY" if q > 0 else "SELL", "quantity": abs(q), "price": px,
                        "exec_id": eid, "client_order_id": cids[s], "position_after": q, "realized_pnl": 0.0})
    journal.append({"t": ms(entry_day, 16) + 6000, "type": "cycle_end", "basket": bid, "state": "IN_POSITION",
                    "position": "LONG", "filled": True, "missed_shares": 0.0})
    (state / "baskets" / f"{bid}.json").write_text(json.dumps({
        "basket_id": bid, "master_state": "IN_POSITION", "position": "LONG", "pending_action": "NONE",
        "decision_day": days[-1], "halt_after_flat": False, "halt_reason": "", "cycle_day": entry_day, "cycle_legs": [],
        "entry": {"decision_day": days[-7], "day": entry_day, "z": -2.11, "restored": False}}))

    # Entries decided at yesterday's close, sent this morning.
    positions = {f"{bid}_{s}": {"symbol": s, "quantity": float(q), "avg_price": px, "realized_pnl": 0.0}
                 for s, (q, px) in legs.items()}
    sent = ms(today, 9) + 31 * 60_000
    for n, (pb, (side, pl, _)) in enumerate(PENDING.items()):
        cl = [{"client_order_id": f"{pb}_{s}#{sent}-{n * 3 + i}", "symbol": s, "position_key": f"{pb}_{s}",
               "side": sd, "target": float(q), "baseline": 0.0} for i, (s, (sd, q)) in enumerate(pl.items())]
        (state / "baskets" / f"{pb}.json").write_text(json.dumps({
            "basket_id": pb, "master_state": "ENTRY_PENDING", "position": side, "pending_action": "NONE",
            "decision_day": days[-1], "halt_after_flat": False, "halt_reason": "", "cycle_day": today, "cycle_legs": cl}))
        if sent <= now * 1000:
            journal.append({"t": sent, "type": "cycle_start", "basket": pb, "label": "ENTRY", "day": today, "position": side,
                            "legs": [{"symbol": c["symbol"], "side": c["side"], "quantity": c["target"],
                                      "client_order_id": c["client_order_id"]} for c in cl]})
    journal.append({"t": sent, "type": "entry_skipped", "basket": "apo_hban_schw",
                    "reason": "SCHW net $8,961 > cap $8,214"})

    if data:  # what the broker view keeps: one commission per fill, ~$1 a leg
        data.mkdir(parents=True, exist_ok=True)
        (data / "commissions.jsonl").write_text("".join(
            json.dumps({"exec_id": f"0000e0d5.{s}.01.01", "commission": 1.0 + 0.05 * n, "currency": "USD", "symbol": s,
                        "order_ref": cids[s], "time": ""}) + "\n" for n, s in enumerate(legs)))

    hb, reason = HALTED
    (state / "baskets" / f"{hb}.json").write_text(json.dumps({
        "basket_id": hb, "master_state": "SYSTEM_HALTED", "position": "FLAT", "pending_action": "NONE",
        "decision_day": days[-1], "halt_after_flat": False, "halt_reason": reason, "cycle_day": 0, "cycle_legs": []}))
    positions[f"{hb}_ED"] = {"symbol": "ED", "quantity": 12.0, "avg_price": 101.4, "realized_pnl": 0.0}
    journal.append({"t": int(now * 1000) - 6 * 3600_000 + 4000, "type": "halt", "basket": hb, "reason": reason})

    (state / "positions.json").write_text(json.dumps({"positions": positions, "seen_exec_ids": seen,
                                                      "total_realized_pnl": 0.0}, indent=2))
    (state / "status.json").write_text(json.dumps({
        "updated_at": int(now * 1000), "started_at": int(now * 1000) - 6 * 3600_000, "running": True,
        "git_sha": "9efb6c1d", "broker_connected": True, "ibkr_connected": True, "ibkr_down_since": None, "equity_usd": 82140.0, "equity_at_start_usd": 82140.0,
        "baskets": 61, "gross_exposure_usd": 4880.0, "reserved_gross_usd": 14960.0,
        "limits": {"max_gross_exposure_usd": 82140.0, "max_symbol_net_usd": 8214.0, "max_basket_loss_usd": 751.6},
        "symbol_net_usd": {"SCHW": 7905.4, "HBAN": 6120.9, "FITB": 2297.4, "TFC": 828.9, "KEY": 1093.1, "TRV": 1352.5,
                           "AJG": 1591.2, "ED": 1216.8, "BALL": 1647.4, "IP": 1720.0, "SW": 1386.9},
        "kill": False, "flatten": False}, indent=2))
    journal.sort(key=lambda e: e["t"])
    (state / "journal.jsonl").write_text("".join(json.dumps(e) + "\n" for e in journal))

    stamp = lambda t: datetime.fromtimestamp(t, NY).strftime("%Y-%m-%d %H:%M:%S.000")
    manual = "[IbkrOrderTracker] Execution 00025b44.6ad7573d.01.01 for untracked order 0 (ref 'RapidOrder', BOT 1 GWW) ignored."
    b0 = before / 1000
    (logs / "funtrade.log").write_text("".join([
        f"[{stamp(b0 + 3)}] [FunTrade] [error] [PortfolioManager] MISMATCH IP: bot books -53.0000, broker holds 0.0000. Baskets trading IP will halt.\n",
        f"[{stamp(b0 + 4)}] [FunTrade] [warning] {manual}\n",
        f"[{stamp(now - 6 * 3600 + 3)}] [FunTrade] [warning] {manual}\n",
        f"[{stamp(now - 4 * 3600)}] [FunTrade] [warning] {manual}\n",
        f"[{stamp(now - 6 * 3600)}] [FunTrade] [info] [LIVE] Running 61 baskets.\n",
        f"[{stamp(now - 6 * 3600 + 4)}] [FunTrade] [error] [PortfolioManager] MISMATCH ED: bot books 12.0000, broker holds 0.0000. Baskets trading ED will halt.\n",
        f"[{stamp(now - 6 * 3600 + 5)}] [FunTrade] [error] [{hb}] SYSTEM HALTED: {reason}\n",
        f"[{stamp(now - 3 * 3600)}] [FunTrade] [warning] [IBKR] 10089 real-time subscription missing for HBAN, using delayed\n",
        f"[{stamp(now - 3 * 3600 + 60)}] [FunTrade] [warning] [apo_hban_schw] Risk rejected entry: SCHW net $8,961 > cap $8,214\n",
    ]))


def heartbeat(state: Path):
    """Keeps the sample's status.json fresh, like a running bot (dev only)."""
    p = state / "status.json"
    while True:
        s = json.loads(p.read_text())
        s["updated_at"] = int(time.time() * 1000)
        p.write_text(json.dumps(s, indent=2))
        time.sleep(30)


if __name__ == "__main__":
    if sys.argv[1] == "heartbeat":
        heartbeat(Path(sys.argv[2]))
    from .settings import load
    write(Path(sys.argv[1]), Path(sys.argv[2]), load().config_dir, data=Path(sys.argv[1]).parent / "dashboard")
    print(f"sample state written to {sys.argv[1]}")
