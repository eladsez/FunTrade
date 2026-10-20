"""Builds the API responses from the bot's files, the journal, the bot's live feed and the
broker snapshot. Pure functions of their inputs (tests drive them with fixture files)."""
import time
from collections import defaultdict
from datetime import datetime
from zoneinfo import ZoneInfo

from . import store

NY = ZoneInfo("America/New_York")
STALE_S = 180          # status.json is rewritten every minute
STOPPED_S = 300        # no heartbeat for this long = the bot isn't running
MARKET_DATA_LINES = 100


def ny_today() -> int:
    """Today's trading-day number (days since the epoch of New York's calendar date)."""
    d = datetime.now(NY).date()
    return (d - store.EPOCH).days


def ny_midnight_ms() -> int:
    now = datetime.now(NY)
    return int(now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)


def bot_health(status: dict | None, now: float | None = None) -> dict:
    now = now or time.time()
    if not status:
        return {"state": "unknown", "running": None, "age_s": None}
    age = now - status.get("updated_at", 0) / 1000
    if not status.get("running", True):
        st = "stopped"
    elif age > STOPPED_S:
        st = "silent"
    elif age > STALE_S:
        st = "stale"
    else:
        st = "running"
    return {"state": st, "running": st in ("running", "stale"), "age_s": round(age)}


def bot_stopped(status: dict | None, now: float | None = None) -> bool:
    """Safe to edit the bot's state files: it said it stopped, or has been silent 5 min."""
    return bot_health(status, now)["state"] in ("stopped", "silent")


class Ctx:
    """One consistent read of every source, for one request."""

    def __init__(self, settings, configs, journal, live, broker):
        self.s = settings
        self.configs = configs
        self.journal = journal
        self.live = live
        self.broker = broker.snapshot if broker else {"connected": False, "disabled": True}
        self.baskets = store.read_baskets(settings.state_dir)
        self.positions = store.read_positions(settings.state_dir)
        self.status = store.read_status(settings.state_dir)
        self.flags = store.control_flags(settings.state_dir)
        self.prices = live.prices() if live else {}

    # --- books ---
    def legs_of(self, basket: str) -> list[dict]:
        cfg = self.configs.get(basket, {})
        out = []
        for sym in cfg.get("tickers", []):
            p = self.positions.get(f"{basket}_{sym}", {})
            qty = p.get("quantity", 0.0)
            avg = p.get("avg_price", 0.0)
            px = self.prices.get(sym)
            upnl = qty * (px - avg) if px and abs(qty) > 1e-9 else (0.0 if abs(qty) <= 1e-9 else None)
            out.append({"symbol": sym, "weight": cfg["weights"].get(sym), "quantity": qty, "avg_price": avg,
                        "price": px, "realized": p.get("realized_pnl", 0.0), "unrealized": upnl})
        return out

    def broker_qty(self) -> dict[str, float] | None:
        if not self.broker.get("connected") and not self.broker.get("positions"):
            return None
        q = defaultdict(float)
        for p in self.broker.get("positions", []):
            if p.get("sec_type", "STK") == "STK":
                q[p["symbol"]] += p["quantity"]
        return q

    def bot_qty(self) -> tuple[dict, dict]:
        q, by = defaultdict(float), defaultdict(dict)
        for key, p in self.positions.items():
            if abs(p.get("quantity", 0.0)) > 1e-9:
                q[p["symbol"]] += p["quantity"]
                by[p["symbol"]][store.basket_of(key)] = p["quantity"]
        return q, by

    # --- per basket ---
    def row(self, bid: str) -> dict:
        cfg = self.configs[bid]
        st = self.baskets.get(bid, {})
        legs = self.legs_of(bid)
        last = self.journal.last_close(bid)
        zh = store.read_zhistory(self.s.state_dir, bid)
        if zh and (not last or zh[-1][0] > last["day"]):
            last = {"day": zh[-1][0], "z": zh[-1][1]}
        state = st.get("master_state", "FLAT")
        held_days = None
        if state in ("IN_POSITION", "EXIT_PENDING"):
            entry = self.journal.last_entry(bid)
            if entry:
                held_days = ny_today() - entry["start"]["day"]
        unreal = [l["unrealized"] for l in legs]
        skipped = self.journal.snapshot_for(bid)["skipped"]
        today = ny_midnight_ms()
        live = self.live.baskets.get(bid, {}) if self.live else {}
        return {
            "id": bid, "tickers": cfg["tickers"], "entry_z": cfg["entry_z"], "exit_z": cfg["exit_z"],
            "window": cfg["window"], "state": state, "position": st.get("position", "FLAT"),
            "pending_action": st.get("pending_action", "NONE"), "halt_reason": st.get("halt_reason") or None,
            "decision_day": store.day_iso(st.get("decision_day")),
            "close_z": last["z"] if last else None, "close_day": store.day_iso(last["day"]) if last else None,
            "intraday_z": live.get("z"),
            "held_days": held_days,
            "realized": sum(l["realized"] for l in legs),
            "unrealized": None if any(u is None for u in unreal) else sum(unreal),
            "in_books": any(abs(l["quantity"]) > 1e-9 for l in legs),
            "working_legs": len(st.get("cycle_legs", [])),
            "skipped_today": next((e["reason"] for e in reversed(skipped) if e["t"] >= today), None),
        }

    def detail(self, bid: str) -> dict:
        cfg = self.configs[bid]
        j = self.journal.snapshot_for(bid)
        bq = self.broker_qty()
        _, by = self.bot_qty()
        legs = self.legs_of(bid)
        for l in legs:
            # The broker's position is per symbol across every basket; compare totals.
            if bq is not None:
                bot_total = sum(by.get(l["symbol"], {}).values())
                l["broker_match"] = abs(bot_total - bq.get(l["symbol"], 0.0)) < 1e-6
            else:
                l["broker_match"] = None
        cycles = []
        for c in j["cycles"]:
            s, e = c["start"], c["end"]
            cycles.append({
                "label": s.get("label"), "position": s.get("position"), "day": store.day_iso(s.get("day")),
                "t": s["t"], "legs": s.get("legs", []), "end_t": e["t"] if e else None,
                "end_state": e.get("state") if e else None, "filled": e.get("filled") if e else None,
                "fills": [{k: f.get(k) for k in ("t", "symbol", "side", "quantity", "price", "exec_id", "realized_pnl")}
                          for f in c["fills"]],
            })
        risk = store.load_risk(self.s.config_dir)
        return {
            **self.row(bid),
            "weights": cfg["weights"], "metrics": cfg["metrics"], "exported_at": cfg["exported_at"],
            "periods": cfg["periods"], "legs": legs,
            "closes": self._closes(bid, j["closes"]),
            "cycles": cycles, "skipped": j["skipped"][-20:], "halts": j["halts"],
            "loss_limit": (self.status or {}).get("limits", {}).get("max_basket_loss_usd"),
            "allocation_fraction": risk.get("allocation_per_basket"),
        }

    def _closes(self, bid: str, journaled: list) -> list:
        """The journal's closes (z, decision, position) over the startup z history: history
        fills the days before the journal began; a journaled day wins."""
        by_day = {d: {"day": d, "z": z, "action": None, "position": None, "state": None}
                  for d, z in store.read_zhistory(self.s.state_dir, bid)}
        for c in journaled:
            by_day[c["day"]] = c
        return [{"day": store.day_iso(c["day"]), "z": c["z"], "action": c["action"], "position": c["position"],
                 "state": c["state"]} for _, c in sorted(by_day.items())]

    # --- pages ---
    def overview(self) -> dict:
        rows = [self.row(b) for b in self.configs]
        status = self.status or {}
        halted = [r for r in rows if r["state"] == "SYSTEM_HALTED"]
        mism = self.orders()["position_mismatches"]
        acct = self.broker.get("account", {})
        today = ny_midnight_ms()
        unreal = [r["unrealized"] for r in rows if r["in_books"]]
        return {
            "bot": bot_health(self.status),
            "broker": {"connected": self.broker.get("connected", False), "checked_at": self.broker.get("checked_at"),
                       "bot_connected": status.get("broker_connected")},
            "live": {"connected": bool(self.live and self.live.connected)},
            "equity_usd": status.get("equity_usd"),
            "net_liquidation": acct.get("NetLiquidation"),
            "realized": sum(p.get("realized_pnl", 0.0) for p in self.positions.values()),
            "unrealized": None if any(u is None for u in unreal) else sum(unreal),
            "in_position": sum(1 for r in rows if r["in_books"]),
            "gross_exposure_usd": status.get("gross_exposure_usd"),
            "reserved_gross_usd": status.get("reserved_gross_usd"),
            "gross_cap_usd": status.get("limits", {}).get("max_gross_exposure_usd"),
            "flags": self.flags,
            "halted": [{"id": r["id"], "reason": r["halt_reason"]} for r in halted],
            "mismatches": mism,
            "working_orders": sum(r["working_legs"] for r in rows),
            "baskets": rows,
            "timeline": [self._event(e) for e in self.journal.recent_events(today)],
            "fills": [self._fill(f) for f in self.journal.recent_fills(8)],
            "first_run": (self.journal.runs[0]["t"] if self.journal.runs else None),
        }

    @staticmethod
    def _fill(f: dict) -> dict:
        return {k: f.get(k) for k in ("t", "basket", "symbol", "side", "quantity", "price", "realized_pnl")}

    @staticmethod
    def _event(e: dict) -> dict:
        kind = e["type"]
        b = e.get("basket")
        if kind == "cycle_start":
            text = f"{b}: {e.get('label', '').lower()} sent, {len(e.get('legs', []))} MOC orders"
        elif kind == "cycle_end":
            text = f"{b}: cycle resolved → {e.get('state')}" + ("" if e.get("filled") else " (no fills)")
        elif kind == "entry_skipped":
            text = f"{b}: entry skipped, {e.get('reason')}"
        elif kind == "halt":
            text = f"{b}: HALTED, {e.get('reason')}"
        elif kind == "start":
            text = f"Bot started ({e.get('baskets')} baskets, {str(e.get('git_sha', ''))[:7]})"
        elif kind == "stop":
            text = "Bot stopped"
        else:
            text = kind
        return {"t": e["t"], "type": kind, "basket": b, "text": text}

    def orders(self) -> dict:
        bot = []
        for bid, st in self.baskets.items():
            for leg in st.get("cycle_legs", []):
                bot.append({"basket": bid, "symbol": leg["symbol"], "side": leg["side"], "quantity": leg["target"],
                            "client_order_id": leg["client_order_id"], "basket_state": st.get("master_state")})
        broker_orders = self.broker.get("orders", [])
        by_ref = {o["order_ref"]: o for o in broker_orders if o.get("order_ref")}
        have_broker = bool(self.broker.get("checked_at"))
        rows, seen = [], set()
        for o in bot:
            b = by_ref.get(o["client_order_id"])
            seen.add(o["client_order_id"])
            if not have_broker:
                match = "unknown"
            elif not b:
                match = "missing_at_broker"
            elif abs(b["quantity"] - o["quantity"]) > 1e-6 or b["side"] != o["side"]:
                match = "differs"
            else:
                match = "match"
            rows.append({**o, "broker": b, "match": match})
        for ref, b in by_ref.items():
            if ref not in seen and "#" in ref:
                rows.append({"basket": store.basket_of(ref.split("#")[0]), "symbol": b["symbol"], "side": b["side"],
                             "quantity": None, "client_order_id": ref, "basket_state": None, "broker": b,
                             "match": "unknown_to_bot"})
        other = [b for b in broker_orders if "#" not in (b.get("order_ref") or "")]

        bq = self.broker_qty()
        botq, by = self.bot_qty()
        positions, mism = [], []
        for sym in sorted(set(botq) | set(bq or {})):
            bv, kv = botq.get(sym, 0.0), (bq.get(sym, 0.0) if bq is not None else None)
            ok = None if kv is None else abs(bv - kv) < 1e-6
            row = {"symbol": sym, "bot": bv, "broker": kv, "baskets": by.get(sym, {}), "match": ok}
            positions.append(row)
            if ok is False:
                mism.append(row)
        seen_ids = set((store.read_json(self.s.state_dir / "positions.json") or {}).get("seen_exec_ids", []))
        execs = [{**e, "booked": e["exec_id"] in seen_ids} for e in self.broker.get("executions", [])]
        return {
            "broker": {k: self.broker.get(k) for k in ("connected", "checked_at", "error")},
            "orders": rows, "other_orders": other, "positions": positions, "position_mismatches": mism,
            "executions": execs,
        }

    def system(self, log_lines: list) -> dict:
        status = self.status or {}
        symbols = sorted({s for c in self.configs.values() for s in c["tickers"]})
        state_files = [p for p in self.s.state_dir.glob("**/*.json")]
        saved = max((p.stat().st_mtime for p in state_files), default=None)
        halts = []
        for bid, st in self.baskets.items():
            if st.get("master_state") != "SYSTEM_HALTED":
                continue
            keys = {k: p.get("quantity", 0.0) for k, p in self.positions.items() if store.basket_of(k) == bid}
            jh = self.journal.snapshot_for(bid)["halts"]
            halts.append({"id": bid, "reason": st.get("halt_reason") or None, "halted_at": jh[-1]["t"] if jh else None,
                          "book_keys": keys})
        return {
            "bot": {**bot_health(self.status), "started_at": status.get("started_at"),
                    "git_sha": status.get("git_sha"), "broker_connected": status.get("broker_connected")},
            "can_edit_state": bot_stopped(self.status),
            "broker": {k: self.broker.get(k) for k in ("connected", "checked_at", "error")},
            "live": {"connected": bool(self.live and self.live.connected)},
            "flags": self.flags,
            "halts": halts,
            "last_close_t": self.journal.latest_close_t(),
            "symbols": len(symbols), "market_data_lines": MARKET_DATA_LINES,
            "state_saved_at": saved,
            "equity_at_start_usd": status.get("equity_at_start_usd"),
            "limits": status.get("limits"),
            "risk_config": store.load_risk(self.s.config_dir),
            "baskets_loaded": status.get("baskets"),
            "baskets_configured": len(self.configs),
            "logs": log_lines,
        }
