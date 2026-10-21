"""Builds the API responses from the bot's files, the journal, the bot's live feed and the
broker snapshot. Pure functions of their inputs (tests drive them with fixture files)."""
import time
from collections import defaultdict
from datetime import datetime
from zoneinfo import ZoneInfo

from . import nyse, store

NY = ZoneInfo("America/New_York")
STALE_S = 180          # status.json is rewritten every minute
STOPPED_S = 300        # no heartbeat for this long = the bot isn't running
MARKET_DATA_LINES = 100
ENTER = ("ENTER_LONG", "ENTER_SHORT")


def ny_day(now: datetime) -> int:
    """The trading-day number of New York's calendar date at `now`."""
    return nyse.day_of(now.astimezone(NY).date())


def ny_midnight_ms(now: datetime) -> int:
    ny = now.astimezone(NY)
    return int(ny.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)


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


def held_qty(snapshot: dict | None) -> dict[str, float] | None:
    """Stock quantity per symbol in a broker snapshot; None if it holds no answer."""
    if not snapshot or (not snapshot.get("connected") and not snapshot.get("positions")):
        return None
    q = defaultdict(float)
    for p in snapshot.get("positions", []):
        if p.get("sec_type", "STK") == "STK":
            q[p["symbol"]] += p["quantity"]
    return q


def fill_cost(side: str, quantity: float, price: float, close: float) -> float:
    """What a fill cost compared with the official close the research assumes (USD; positive
    = worse than the close: bought above it, or sold below it)."""
    return (price - close) * quantity if side == "BUY" else (close - price) * quantity


class Ctx:
    """One consistent read of every source, for one request."""

    def __init__(self, settings, configs, journal, live, broker, now: datetime | None = None):
        self.s = settings
        self.configs = configs
        self.journal = journal
        self.live = live
        self.now = now or datetime.now(NY)
        self.broker = broker.snapshot if broker else {"connected": False, "disabled": True}
        self.broker_prev = getattr(broker, "previous", None) if broker else None
        self.baskets = store.read_baskets(settings.state_dir)
        self.positions = store.read_positions(settings.state_dir)
        self.status = store.read_status(settings.state_dir)
        self.flags = store.control_flags(settings.state_dir)
        self.quotes = live.quotes() if live else {}
        self.today = ny_day(self.now)
        self.last_closed = nyse.last_closed_session(self.now)
        # The broker view keeps them in memory; without one (disabled, tests) read its file.
        self.commissions = (broker.commissions if broker is not None and hasattr(broker, "commissions")
                            else store.load_commissions(settings.data_dir / "commissions.jsonl"))
        self._close_maps: dict[str, dict[int, dict]] = {}
        self._comm_by_basket: dict[str, dict] | None = None
        self._orders: dict | None = None

    # --- journal lookups ---
    def close_map(self, basket: str) -> dict[int, dict]:
        """Journaled closes by day (z, decision, each leg's close)."""
        if basket not in self._close_maps:
            self._close_maps[basket] = {c["day"]: c for c in self.journal.snapshot_for(basket)["closes"]}
        return self._close_maps[basket]

    def official_close(self, basket: str, symbol: str, t_ms: int) -> float | None:
        """The close of the day of a fill at t_ms (the bot's daily bar, journaled at its EOD)."""
        day = ny_day(datetime.fromtimestamp(t_ms / 1000, NY))
        return ((self.close_map(basket).get(day) or {}).get("closes") or {}).get(symbol)

    # --- commissions ---
    def _owners(self) -> dict[str, str]:
        """exec_id -> basket: the journal's fills, else the orderRef (`<basket>_<SYM>#...`)."""
        out = {}
        for exec_id, c in self.commissions.items():
            ref = c.get("order_ref") or ""
            if "#" in ref:
                out[exec_id] = store.basket_of(ref.split("#")[0])
        for f in self.journal.all_fills():
            if f.get("exec_id"):
                out[f["exec_id"]] = f["basket"]
        return out

    def commissions_by_basket(self) -> dict[str, dict]:
        """basket -> {usd, fills, other: {currency: amount}} over the commissions on record."""
        if self._comm_by_basket is None:
            out: dict[str, dict] = {}
            owner = self._owners()
            for exec_id, c in self.commissions.items():
                b = owner.get(exec_id)
                if b is None:
                    continue
                agg = out.setdefault(b, {"usd": 0.0, "fills": 0, "other": {}})
                agg["fills"] += 1
                cur = c.get("currency") or "USD"
                if cur == "USD":
                    agg["usd"] += c["commission"]
                else:
                    agg["other"][cur] = agg["other"].get(cur, 0.0) + c["commission"]
            self._comm_by_basket = out
        return self._comm_by_basket

    def commission_of(self, exec_id: str | None) -> float | None:
        c = self.commissions.get(exec_id or "")
        return c["commission"] if c and (c.get("currency") or "USD") == "USD" else None

    # --- books ---
    def leg_price(self, basket: str, symbol: str) -> dict:
        """The price open P&L uses, and where it comes from: the day's close once the bot has
        journaled it (at its EOD fetch); before that the bot's live feed, which is IBKR delayed
        data (~15 min behind in session); with neither, the latest journaled close."""
        last = self.journal.last_close(basket)
        close_px = ((last or {}).get("closes") or {}).get(symbol)
        q = self.quotes.get(symbol)
        as_close = lambda px, day: {"price": px, "source": "close", "price_day": day, "price_t": None}
        if close_px and last["day"] >= self.last_closed:
            return as_close(close_px, store.day_iso(last["day"]))
        if q and q.get("t"):
            return {"price": q["px"], "source": "delayed", "price_day": None, "price_t": int(q["t"] * 1000)}
        if close_px:
            return as_close(close_px, store.day_iso(last["day"]))
        if q:  # the bot's startup seed: the last close it warmed up on
            return as_close(q["px"], None)
        return {"price": None, "source": None, "price_day": None, "price_t": None}

    def legs_of(self, basket: str) -> list[dict]:
        cfg = self.configs.get(basket, {})
        out = []
        for sym in cfg.get("tickers", []):
            p = self.positions.get(f"{basket}_{sym}", {})
            qty = p.get("quantity", 0.0)
            avg = p.get("avg_price", 0.0)
            px = self.leg_price(basket, sym)
            held = abs(qty) > 1e-9
            upnl = (qty * (px["price"] - avg) if px["price"] else None) if held else 0.0
            out.append({"symbol": sym, "weight": cfg["weights"].get(sym), "quantity": qty, "avg_price": avg,
                        **px, "realized": p.get("realized_pnl", 0.0), "unrealized": upnl})
        return out

    def bot_qty(self) -> tuple[dict, dict]:
        q, by = defaultdict(float), defaultdict(dict)
        for key, p in self.positions.items():
            if abs(p.get("quantity", 0.0)) > 1e-9:
                q[p["symbol"]] += p["quantity"]
                by[p["symbol"]][store.basket_of(key)] = p["quantity"]
        return q, by

    # --- per basket ---
    def entry_of(self, bid: str, st: dict) -> dict | None:
        """The entry that opened the current position: the bot's state file (which records it
        since Phase 1), else the journal's last ENTRY cycle and the decision before it."""
        e = st.get("entry")
        if isinstance(e, dict) and e.get("day"):
            return {"day": e["day"], "decision_day": e.get("decision_day"), "z": e.get("z"),
                    "restored": bool(e.get("restored"))}
        c = self.journal.last_entry(bid)
        if not c or not c["start"].get("day"):
            return None
        day = c["start"]["day"]
        closes = self.close_map(bid)
        decided = max((d for d, x in closes.items() if d < day and x.get("action") in ENTER), default=None)
        return {"day": day, "decision_day": decided, "z": closes[decided]["z"] if decided else None, "restored": False}

    def row(self, bid: str) -> dict:
        cfg = self.configs[bid]
        st = self.baskets.get(bid, {})
        legs = self.legs_of(bid)
        last = self.journal.last_close(bid)
        zh = store.read_zhistory(self.s.state_dir, bid)
        if zh and (not last or zh[-1][0] > last["day"]):
            last = {"day": zh[-1][0], "z": zh[-1][1]}
        state = st.get("master_state", "FLAT")
        entry = self.entry_of(bid, st) if state not in ("FLAT", "ENTRY_PENDING") else None
        unreal = [l["unrealized"] for l in legs]
        skipped = self.journal.snapshot_for(bid)["skipped"]
        today = ny_midnight_ms(self.now)
        live = self.live.baskets.get(bid, {}) if self.live else {}
        comm = self.commissions_by_basket().get(bid)
        return {
            "id": bid, "tickers": cfg["tickers"], "entry_z": cfg["entry_z"], "exit_z": cfg["exit_z"],
            "window": cfg["window"], "state": state, "position": st.get("position", "FLAT"),
            "pending_action": st.get("pending_action", "NONE"), "halt_reason": st.get("halt_reason") or None,
            "decision_day": store.day_iso(st.get("decision_day")),
            "close_z": last["z"] if last else None, "close_day": store.day_iso(last["day"]) if last else None,
            "intraday_z": live.get("z"),
            "entry": {"day": store.day_iso(entry["day"]), "decision_day": store.day_iso(entry["decision_day"]),
                      "z": entry["z"], "restored": entry["restored"]} if entry else None,
            # sessions closed since the entry's own (the day after an entry: 1)
            "held_days": nyse.sessions_between(entry["day"], self.today) if entry else None,
            "realized": sum(l["realized"] for l in legs),
            "unrealized": None if any(u is None for u in unreal) else sum(unreal),
            "commissions": comm["usd"] if comm else None,
            "in_books": any(abs(l["quantity"]) > 1e-9 for l in legs),
            "working_legs": len(st.get("cycle_legs", [])),
            "skipped_today": next((e["reason"] for e in reversed(skipped) if e["t"] >= today), None),
        }

    def _fill(self, basket: str, f: dict) -> dict:
        close = self.official_close(basket, f["symbol"], f["t"])
        cost = fill_cost(f["side"], f["quantity"], f["price"], close) if close else None
        return {**{k: f.get(k) for k in ("t", "symbol", "side", "quantity", "price", "exec_id", "realized_pnl")},
                "basket": basket, "close": close, "cost_usd": cost,
                "cost_bps": cost / (close * f["quantity"]) * 1e4 if cost is not None and f["quantity"] else None,
                "commission": self.commission_of(f.get("exec_id"))}

    @staticmethod
    def fill_quality(fills: list[dict]) -> dict:
        """Fills vs the official close: how many could be compared (the close is journaled at
        the bot's EOD, ~2 h after the fills), their total cost, the notional-weighted average
        in basis points (positive = worse than the close) and the worst fill."""
        done = [f for f in fills if f["cost_usd"] is not None]
        notional = sum(f["close"] * f["quantity"] for f in done)
        cost = sum(f["cost_usd"] for f in done)
        worst = max(done, key=lambda f: f["cost_bps"], default=None)
        return {"fills": len(done), "pending": len(fills) - len(done), "cost_usd": cost if done else None,
                "bps": cost / notional * 1e4 if notional else None,
                "worst": {k: worst[k] for k in ("basket", "symbol", "cost_bps", "t")} if worst else None}

    def detail(self, bid: str) -> dict:
        cfg = self.configs[bid]
        j = self.journal.snapshot_for(bid)
        status = {p["symbol"]: p for p in self.orders()["positions"]}
        legs = self.legs_of(bid)
        for l in legs:
            # The broker's position is per symbol across every basket; compare totals.
            p = status.get(l["symbol"])
            l["broker_match"] = p["match"] if p else None
            l["broker_settling"] = bool(p and p["settling"])
        cycles, fills = [], []
        for c in j["cycles"]:
            s, e = c["start"], c["end"]
            cf = [self._fill(bid, f) for f in c["fills"]]
            fills += cf
            cycles.append({
                "label": s.get("label"), "position": s.get("position"), "day": store.day_iso(s.get("day")),
                "t": s["t"], "legs": s.get("legs", []), "end_t": e["t"] if e else None,
                "end_state": e.get("state") if e else None, "filled": e.get("filled") if e else None,
                "fills": cf,
            })
        risk = store.load_risk(self.s.config_dir)
        return {
            **self.row(bid),
            "weights": cfg["weights"], "metrics": cfg["metrics"], "exported_at": cfg["exported_at"],
            "periods": cfg["periods"], "legs": legs,
            "closes": self._closes(bid, j["closes"]),
            "cycles": cycles, "skipped": j["skipped"][-20:], "halts": j["halts"],
            "fill_quality": self.fill_quality(fills),
            "commission_fills": (self.commissions_by_basket().get(bid) or {}).get("fills", 0),
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
        orders = self.orders()
        acct = self.broker.get("account", {})
        today = ny_midnight_ms(self.now)
        unreal = [r["unrealized"] for r in rows if r["in_books"]]
        comm = self.commissions_by_basket()
        held = [l for r in rows if r["in_books"] for l in self.legs_of(r["id"]) if abs(l["quantity"]) > 1e-9]
        return {
            "bot": bot_health(self.status),
            "broker": {"connected": self.broker.get("connected", False), "checked_at": self.broker.get("checked_at"),
                       "bot_connected": status.get("broker_connected")},
            "live": {"connected": bool(self.live and self.live.connected)},
            "equity_usd": status.get("equity_usd"),
            "net_liquidation": acct.get("NetLiquidation"),
            "realized": sum(p.get("realized_pnl", 0.0) for p in self.positions.values()),
            "unrealized": None if any(u is None for u in unreal) else sum(unreal),
            "prices": self.price_summary(held),
            "commissions": sum(c["usd"] for c in comm.values()) if comm else None,
            "commission_fills": sum(c["fills"] for c in comm.values()),
            "in_position": sum(1 for r in rows if r["in_books"]),
            "gross_exposure_usd": status.get("gross_exposure_usd"),
            "reserved_gross_usd": status.get("reserved_gross_usd"),
            "gross_cap_usd": status.get("limits", {}).get("max_gross_exposure_usd"),
            "flags": self.flags,
            "halted": [{"id": r["id"], "reason": r["halt_reason"]} for r in halted],
            "mismatches": orders["position_mismatches"],
            "settling": orders["position_settling"],
            "working_orders": sum(r["working_legs"] for r in rows),
            "baskets": rows,
            "timeline": [self._event(e) for e in self.journal.recent_events(today)],
            "fills": [self._fill(f["basket"], f) for f in self.journal.recent_fills(8)],
            "fill_quality": self.fill_quality([self._fill(f["basket"], f) for f in self.journal.all_fills()]),
            "first_run": (self.journal.runs[0]["t"] if self.journal.runs else None),
        }

    @staticmethod
    def price_summary(legs: list[dict]) -> dict:
        """What open P&L is priced with: closes (of which day), delayed quotes (the oldest
        one's time), a mix, or nothing (no legs held)."""
        srcs = {l["source"] for l in legs if l["source"]}
        delayed = [l["price_t"] for l in legs if l["source"] == "delayed"]
        days = sorted({l["price_day"] for l in legs if l["source"] == "close" and l["price_day"]})
        return {"source": None if not srcs else srcs.pop() if len(srcs) == 1 else "mixed",
                "oldest_t": min(delayed) if delayed else None, "close_day": days[-1] if days else None,
                "missing": sum(1 for l in legs if l["source"] is None)}

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

    def books_changed_at(self) -> float:
        """When the bot's books last changed (epoch s): its latest journaled fill, or the
        positions file's write (fills booked before the journal existed)."""
        t = (self.journal.latest_fill_t() or 0) / 1000
        try:
            t = max(t, (self.s.state_dir / "positions.json").stat().st_mtime)
        except FileNotFoundError:
            pass
        return t

    def orders(self) -> dict:
        if self._orders is None:
            self._orders = self._build_orders()
        return self._orders

    def _build_orders(self) -> dict:
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

        # A difference is a mismatch once two broker snapshots, both taken after the books
        # last changed, show it. Until then it's "settling": around the 16:00 fills the broker
        # snapshot (polled every minute) can simply be behind the books, or ahead of them.
        bq = held_qty(self.broker)
        changed = self.books_changed_at()
        after = lambda snap: (snap or {}).get("checked_at") or 0
        pq = held_qty(self.broker_prev) if after(self.broker_prev) > changed and after(self.broker) > changed else None
        botq, by = self.bot_qty()
        positions, mism, settling = [], [], []
        for sym in sorted(set(botq) | set(bq or {})):
            bv, kv = botq.get(sym, 0.0), (bq.get(sym, 0.0) if bq is not None else None)
            ok = None if kv is None else abs(bv - kv) < 1e-6
            confirmed = ok is False and pq is not None and abs(bv - pq.get(sym, 0.0)) >= 1e-6
            row = {"symbol": sym, "bot": bv, "broker": kv, "baskets": by.get(sym, {}), "match": ok,
                   "settling": ok is False and not confirmed}
            positions.append(row)
            if confirmed:
                mism.append(row)
            elif ok is False:
                settling.append(row)
        seen_ids = set((store.read_json(self.s.state_dir / "positions.json") or {}).get("seen_exec_ids", []))
        execs = []
        for e in self.broker.get("executions", []):
            # Bot orders carry `<basket>_<SYM>#...` in orderRef; anything else was placed by
            # hand (TWS, the mobile app) and is not the bot's to book.
            bots = "#" in (e.get("order_ref") or "")
            execs.append({**e, "booked": e["exec_id"] in seen_ids, "origin": "bot" if bots else "manual",
                          "commission": self.commission_of(e["exec_id"])})
        return {
            "broker": {k: self.broker.get(k) for k in ("connected", "checked_at", "error")},
            "orders": rows, "other_orders": other, "positions": positions, "position_mismatches": mism,
            "position_settling": settling, "executions": execs,
        }

    def symbol_caps(self) -> list[dict]:
        """Net USD per symbol (held + reserved by working entries) against the per-symbol cap,
        fullest first, with how many configured baskets trade the symbol."""
        status = self.status or {}
        cap = (status.get("limits") or {}).get("max_symbol_net_usd")
        users = defaultdict(int)
        for cfg in self.configs.values():
            for s in cfg["tickers"]:
                users[s] += 1
        rows = [{"symbol": s, "usd": v, "pct": v / cap if cap else None, "baskets": users.get(s, 0)}
                for s, v in (status.get("symbol_net_usd") or {}).items()]
        return sorted(rows, key=lambda r: -r["usd"])

    def system(self, log_runs: list) -> dict:
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
            "symbol_caps": self.symbol_caps() if "symbol_net_usd" in status else None,
            "risk_config": store.load_risk(self.s.config_dir),
            "baskets_loaded": status.get("baskets"),
            "baskets_configured": len(self.configs),
            "logs": log_runs,
        }
