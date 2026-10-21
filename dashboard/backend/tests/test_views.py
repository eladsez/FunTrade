import json
import time
from datetime import datetime, timedelta

import pytest

from app import nyse, store, views


class FakeBroker:
    def __init__(self, snapshot):
        self.snapshot = snapshot
        self.version = 1


def broker_matching(cfg, **over):
    """A broker that holds what the sample's books say and has every working order."""
    pos = json.loads((cfg.state_dir / "positions.json").read_text())["positions"]
    held = {}
    for p in pos.values():
        held[p["symbol"]] = held.get(p["symbol"], 0) + p["quantity"]
    orders = []
    for f in (cfg.state_dir / "baskets").glob("*.json"):
        for leg in json.loads(f.read_text()).get("cycle_legs", []):
            orders.append({"symbol": leg["symbol"], "side": leg["side"], "quantity": leg["target"],
                           "order_ref": leg["client_order_id"], "status": "PreSubmitted", "type": "MOC"})
    snap = {"connected": True, "checked_at": time.time(), "orders": orders,
            "positions": [{"symbol": s, "quantity": q, "sec_type": "STK"} for s, q in held.items()],
            "executions": [], "account": {"NetLiquidation": {"value": 301450.0, "currency": "ILS"}}}
    snap.update(over)
    return FakeBroker(snap)


def ctx(cfg, journal, broker=None):
    return views.Ctx(cfg, store.load_configs(cfg.config_dir), journal, None, broker)


def test_overview_summarizes_the_sample(cfg, journal):
    o = ctx(cfg, journal).overview()
    rows = {r["id"]: r for r in o["baskets"]}
    assert len(rows) == 61
    assert rows["fitb_key_tfc"]["state"] == "IN_POSITION"
    assert rows["fitb_key_tfc"]["held_days"] >= 6
    assert rows["fitb_key_tfc"]["close_z"] is not None
    assert rows["hban_schw_trv"]["working_legs"] == 3
    assert o["halted"] == [{"id": "cms_ed_ppl", "reason": rows["cms_ed_ppl"]["halt_reason"]}]
    assert o["bot"]["state"] == "running"
    assert o["equity_usd"] == 82140.0
    assert o["unrealized"] is not None, "no live feed: priced at the last journaled close"
    assert o["prices"]["source"] == "close" and o["prices"]["missing"] == 0


def test_orders_match_by_order_ref(cfg, journal):
    o = ctx(cfg, journal, broker_matching(cfg)).orders()
    assert o["orders"] and all(r["match"] == "match" for r in o["orders"])
    assert o["position_mismatches"] == []


def test_orders_report_every_kind_of_difference(cfg, journal):
    b = broker_matching(cfg)
    snap = b.snapshot
    missing = snap["orders"].pop(0)                       # bot thinks it's working, broker doesn't have it
    snap["orders"][0] = {**snap["orders"][0], "quantity": snap["orders"][0]["quantity"] + 1}
    snap["orders"].append({"symbol": "AAPL", "side": "BUY", "quantity": 1, "status": "Submitted",
                           "order_ref": "aapl_mu_wdc_AAPL#123-9", "type": "MOC"})
    snap["orders"].append({"symbol": "SPY", "side": "BUY", "quantity": 1, "status": "Submitted",
                           "order_ref": "", "type": "LMT"})
    snap["positions"] = [p for p in snap["positions"] if p["symbol"] != "ED"]
    o = ctx(cfg, journal, b).orders()
    by_ref = {r["client_order_id"]: r["match"] for r in o["orders"]}
    assert by_ref[missing["order_ref"]] == "missing_at_broker"
    assert "differs" in by_ref.values()
    assert by_ref["aapl_mu_wdc_AAPL#123-9"] == "unknown_to_bot"
    assert [x["symbol"] for x in o["other_orders"]] == ["SPY"]
    assert o["position_mismatches"] == [], "one snapshot is not enough to call it a mismatch"
    assert [m["symbol"] for m in o["position_settling"]] == ["ED"]
    assert o["position_settling"][0]["broker"] == 0.0


def test_without_a_broker_matches_are_unknown_not_failures(cfg, journal):
    o = ctx(cfg, journal).orders()
    assert {r["match"] for r in o["orders"]} == {"unknown"}
    assert o["position_mismatches"] == []


def test_executions_marked_booked_from_seen_exec_ids(cfg, journal):
    b = broker_matching(cfg, executions=[{"exec_id": "0000e0d5.FITB.01.01", "symbol": "FITB", "time": "1"},
                                         {"exec_id": "new.1", "symbol": "HBAN", "time": "2"}])
    ex = {e["exec_id"]: e["booked"] for e in ctx(cfg, journal, b).orders()["executions"]}
    assert ex == {"0000e0d5.FITB.01.01": True, "new.1": False}


def test_basket_detail_has_history_and_legs(cfg, journal):
    d = ctx(cfg, journal, broker_matching(cfg)).detail("fitb_key_tfc")
    assert len(d["closes"]) == 90
    assert d["cycles"][0]["label"] == "ENTRY" and len(d["cycles"][0]["fills"]) == 3
    assert [l["symbol"] for l in d["legs"]] == ["FITB", "KEY", "TFC"]
    assert all(l["broker_match"] for l in d["legs"])


def test_bot_health_from_heartbeat():
    now = time.time()
    assert views.bot_health(None)["state"] == "unknown"
    assert views.bot_health({"updated_at": now * 1000, "running": True}, now)["state"] == "running"
    assert views.bot_health({"updated_at": (now - 200) * 1000, "running": True}, now)["state"] == "stale"
    assert views.bot_health({"updated_at": (now - 400) * 1000, "running": True}, now)["state"] == "silent"
    assert views.bot_health({"updated_at": now * 1000, "running": False}, now)["state"] == "stopped"
    assert not views.bot_stopped(None), "no status file: can't tell, so not safe"
    assert views.bot_stopped({"updated_at": now * 1000, "running": False}, now)


def test_system_lists_halts_with_their_books(cfg, journal):
    s = ctx(cfg, journal).system([])
    assert s["halts"][0]["id"] == "cms_ed_ppl"
    assert s["halts"][0]["book_keys"] == {"cms_ed_ppl_ED": 12.0}
    assert s["can_edit_state"] is False
    assert s["symbols"] > 50


def test_startup_z_history_fills_days_before_the_journal(cfg, journal):
    first = journal.closes["fitb_key_tfc"][0]["day"]
    zdir = cfg.state_dir / "zhistory"
    zdir.mkdir()
    # 30 older days, plus one that overlaps the journal (the journal's value must win)
    days = [[first - 30 + i, 0.1 * i] for i in range(30)] + [[first, 9.9]]
    (zdir / "fitb_key_tfc.json").write_text(json.dumps({"basket": "fitb_key_tfc", "window": 36, "days": days}))
    (zdir / "aapl_mu_wdc.json").write_text(json.dumps({"basket": "aapl_mu_wdc", "days": [[99999, 1.23]]}))
    c = ctx(cfg, journal)
    d = c.detail("fitb_key_tfc")
    assert len(d["closes"]) == 120
    assert d["closes"][0]["action"] is None and d["closes"][0]["position"] is None
    assert d["closes"][30]["z"] != 9.9
    assert c.row("aapl_mu_wdc")["close_z"] == 1.23, "a newer history day beats an older journaled close"
    assert c.row("aapl_mu_wdc")["close_day"] == store.day_iso(99999)


# --- Phase 1: accuracy ---

NY = views.NY


class FakeLive:
    connected = True

    def __init__(self, quotes):
        self._q = quotes
        self.baskets = {}

    def quotes(self):
        return self._q


def basket_file(cfg, bid):
    return cfg.state_dir / "baskets" / f"{bid}.json"


def edit_basket(cfg, bid, **fields):
    p = basket_file(cfg, bid)
    st = json.loads(p.read_text())
    st.update(fields)
    p.write_text(json.dumps(st))


def test_entry_comes_from_the_state_file(cfg, journal):
    r = ctx(cfg, journal).row("fitb_key_tfc")
    assert r["entry"]["z"] == -2.11 and r["entry"]["restored"] is False
    assert r["entry"]["day"] == store.day_iso(journal.cycles["fitb_key_tfc"][0]["start"]["day"])


def test_entry_falls_back_to_the_journal(cfg, journal):
    st = json.loads(basket_file(cfg, "fitb_key_tfc").read_text())
    del st["entry"]
    basket_file(cfg, "fitb_key_tfc").write_text(json.dumps(st))
    r = ctx(cfg, journal).row("fitb_key_tfc")
    entry_day = journal.cycles["fitb_key_tfc"][0]["start"]["day"]
    assert r["entry"]["day"] == store.day_iso(entry_day)
    decided = [c for c in journal.closes["fitb_key_tfc"] if c["action"] == "ENTER_LONG"][-1]
    assert r["entry"]["decision_day"] == store.day_iso(decided["day"]) and r["entry"]["z"] == decided["z"]


def test_restored_entry_is_flagged(cfg, journal):
    edit_basket(cfg, "fitb_key_tfc", entry={"decision_day": 20745, "day": 20746, "z": -2.37, "restored": True})
    r = ctx(cfg, journal).row("fitb_key_tfc")
    assert r["entry"] == {"day": "2026-10-20", "decision_day": "2026-10-19", "z": -2.37, "restored": True}


def test_no_entry_for_flat_or_entering_baskets(cfg, journal):
    c = ctx(cfg, journal)
    assert c.row("aapl_mu_wdc")["entry"] is None and c.row("aapl_mu_wdc")["held_days"] is None
    assert c.row("hban_schw_trv")["entry"] is None, "ENTRY_PENDING: not held yet"


def test_held_counts_trading_sessions(cfg, journal):
    edit_basket(cfg, "fitb_key_tfc", entry={"decision_day": nyse.day_of(datetime(2026, 11, 24).date()),
                                             "day": nyse.day_of(datetime(2026, 11, 25).date()), "z": -2.2})
    monday = datetime(2026, 11, 30, 10, tzinfo=NY)
    assert views.Ctx(cfg, store.load_configs(cfg.config_dir), journal, None, None, now=monday).row(
        "fitb_key_tfc")["held_days"] == 2, "Thanksgiving and the weekend aren't held days"


def last_close_day(journal, bid="fitb_key_tfc"):
    return journal.closes[bid][-1]["day"]


def at_day(day, hour):
    return datetime.combine(nyse.date_of(day), datetime.min.time(), NY) + timedelta(hours=hour)


def leg(c, sym, bid="fitb_key_tfc"):
    return next(l for l in c.legs_of(bid) if l["symbol"] == sym)


def test_after_the_bots_eod_the_close_prices_open_pnl(cfg, journal):
    d = last_close_day(journal)
    live = FakeLive({"FITB": {"px": 50.0, "t": at_day(d, 19).timestamp()}})
    c = views.Ctx(cfg, store.load_configs(cfg.config_dir), journal, live, None, now=at_day(d, 19))
    fitb = leg(c, "FITB")
    assert fitb["source"] == "close" and fitb["price_day"] == store.day_iso(d)
    assert fitb["price"] == journal.closes["fitb_key_tfc"][-1]["closes"]["FITB"], "the close, not the feed"
    assert fitb["unrealized"] == pytest.approx(52 * (fitb["price"] - 44.18))


def test_in_session_the_delayed_feed_prices_open_pnl(cfg, journal):
    d = last_close_day(journal)
    now = at_day(d, 24 * 7 + 11)            # a week later, mid-morning: the journaled close is old
    t = now.timestamp() - 60
    live = FakeLive({"FITB": {"px": 50.0, "t": t}})
    c = views.Ctx(cfg, store.load_configs(cfg.config_dir), journal, live, None, now=now)
    fitb = leg(c, "FITB")
    assert fitb == {**fitb, "price": 50.0, "source": "delayed", "price_t": int(t * 1000), "price_day": None}
    key = leg(c, "KEY")
    assert key["source"] == "close" and key["price_day"] == store.day_iso(d), "no quote: the latest close"
    assert c.price_summary([fitb, key])["source"] == "mixed"


def test_without_closes_the_startup_seed_or_nothing(cfg, tmp_path):
    empty = store.JournalIndex(tmp_path / "none.jsonl")
    live = FakeLive({"FITB": {"px": 44.0, "t": None}})
    c = views.Ctx(cfg, store.load_configs(cfg.config_dir), empty, live, None)
    assert leg(c, "FITB")["source"] == "close" and leg(c, "FITB")["price"] == 44.0
    assert leg(c, "KEY")["source"] is None and leg(c, "KEY")["unrealized"] is None
    assert c.row("fitb_key_tfc")["unrealized"] is None, "one leg unpriced: the basket's open P&L is unknown"


def test_price_summary():
    s = views.Ctx.price_summary
    assert s([]) == {"source": None, "oldest_t": None, "close_day": None, "missing": 0}
    legs = [{"source": "delayed", "price_t": 5, "price_day": None}, {"source": "delayed", "price_t": 3, "price_day": None},
            {"source": None, "price_t": None, "price_day": None}]
    assert s(legs) == {"source": "delayed", "oldest_t": 3, "close_day": None, "missing": 1}


def test_commissions_are_attributed_to_baskets(cfg, journal):
    with (cfg.data_dir / "commissions.jsonl").open("a") as f:
        f.write(json.dumps({"exec_id": "x.1", "commission": 4.0, "currency": "ILS", "symbol": "HBAN",
                            "order_ref": "hban_schw_trv_HBAN#1-1"}) + "\n")
        f.write(json.dumps({"exec_id": "x.2", "commission": 1.0, "currency": "USD", "symbol": "GWW",
                            "order_ref": "RapidOrder"}) + "\n")
        f.write('{"exec_id": "torn')
    c = ctx(cfg, journal)
    assert c.row("fitb_key_tfc")["commissions"] == pytest.approx(3.15)
    assert c.commissions_by_basket()["hban_schw_trv"] == {"usd": 0.0, "fills": 1, "other": {"ILS": 4.0}}
    o = c.overview()
    assert o["commissions"] == pytest.approx(3.15) and o["commission_fills"] == 4, "a manual trade isn't a basket's"
    d = c.detail("fitb_key_tfc")
    assert [f["commission"] for f in d["cycles"][0]["fills"]] == [1.0, 1.05, 1.1]
    assert d["commission_fills"] == 3


def test_commissions_from_the_live_broker_view(cfg, journal):
    b = broker_matching(cfg)
    b.commissions = {"0000e0d5.FITB.01.01": {"commission": 2.5, "currency": "USD", "order_ref": ""}}
    assert ctx(cfg, journal, b).row("fitb_key_tfc")["commissions"] == 2.5, "attributed through the journal's fill"


def test_fills_are_compared_with_the_official_close(cfg, journal):
    d = ctx(cfg, journal).detail("fitb_key_tfc")
    by = {f["symbol"]: f for f in d["cycles"][0]["fills"]}
    assert by["FITB"]["close"] == 44.15 and by["FITB"]["cost_usd"] == pytest.approx(52 * 0.03), "bought above the close"
    assert by["KEY"]["cost_usd"] == pytest.approx(-61 * 0.03), "sold above the close: better than the research"
    assert by["FITB"]["cost_bps"] == pytest.approx(0.03 / 44.15 * 1e4)
    q = d["fill_quality"]
    notional = 44.15 * 52 + 17.89 * 61 + 46.02 * 18
    assert q["fills"] == 3 and q["pending"] == 0
    assert q["cost_usd"] == pytest.approx(0.03 * (52 - 61 + 18))
    assert q["bps"] == pytest.approx(q["cost_usd"] / notional * 1e4)
    assert q["worst"]["symbol"] == "FITB"
    assert ctx(cfg, journal).overview()["fill_quality"]["fills"] == 3


def test_fill_without_a_journaled_close_is_pending():
    q = views.Ctx.fill_quality([{"cost_usd": None, "close": None, "quantity": 1, "cost_bps": None}])
    assert q == {"fills": 0, "pending": 1, "cost_usd": None, "bps": None, "worst": None}
    assert views.fill_cost("BUY", 10, 10.5, 10.0) == 5.0
    assert views.fill_cost("SELL", 10, 10.5, 10.0) == -5.0


def test_a_mismatch_needs_two_snapshots_after_the_books_changed(cfg, journal):
    b = broker_matching(cfg)
    b.snapshot["positions"] = [p for p in b.snapshot["positions"] if p["symbol"] != "ED"]
    changed = (cfg.state_dir / "positions.json").stat().st_mtime
    b.snapshot["checked_at"] = changed + 60
    b.previous = {**b.snapshot, "checked_at": changed - 1}
    o = ctx(cfg, journal, b).orders()
    assert o["position_mismatches"] == [] and o["position_settling"][0]["symbol"] == "ED", \
        "the earlier snapshot predates the books: still settling"

    b.previous = {**b.snapshot, "checked_at": changed + 1}
    c = ctx(cfg, journal, b)
    assert [m["symbol"] for m in c.orders()["position_mismatches"]] == ["ED"]
    assert c.overview()["mismatches"][0]["symbol"] == "ED"
    ed = next(l for l in c.detail("cms_ed_ppl")["legs"] if l["symbol"] == "ED")
    assert ed["broker_match"] is False and ed["broker_settling"] is False

    b.previous = {**b.snapshot, "checked_at": changed + 1,
                  "positions": b.snapshot["positions"] + [{"symbol": "ED", "quantity": 12.0, "sec_type": "STK"}]}
    assert ctx(cfg, journal, b).orders()["position_settling"][0]["symbol"] == "ED", "the snapshot before agreed"


def test_executions_say_who_placed_them(cfg, journal):
    b = broker_matching(cfg, executions=[{"exec_id": "e1", "symbol": "FITB", "order_ref": "fitb_key_tfc_FITB#1-0"},
                                         {"exec_id": "e2", "symbol": "GWW", "order_ref": "RapidOrder"}])
    ex = {e["exec_id"]: e["origin"] for e in ctx(cfg, journal, b).orders()["executions"]}
    assert ex == {"e1": "bot", "e2": "manual"}


def test_symbol_caps(cfg, journal):
    s = ctx(cfg, journal).system([])
    caps = s["symbol_caps"]
    assert caps[0] == {"symbol": "SCHW", "usd": 7905.4, "pct": pytest.approx(7905.4 / 8214.0), "baskets": caps[0]["baskets"]}
    assert caps[0]["baskets"] > 5, "SCHW is in many baskets"
    assert [c["usd"] for c in caps] == sorted((c["usd"] for c in caps), reverse=True)
    st = json.loads((cfg.state_dir / "status.json").read_text())
    del st["symbol_net_usd"]
    (cfg.state_dir / "status.json").write_text(json.dumps(st))
    assert ctx(cfg, journal).system([])["symbol_caps"] is None, "an older bot doesn't report it"


def test_timeline_texts():
    ev = views.Ctx._event
    assert ev({"t": 1, "type": "cycle_end", "basket": "b", "state": "FLAT", "filled": False})["text"] == \
        "b: cycle resolved → FLAT (no fills)"
    assert ev({"t": 1, "type": "stop"})["text"] == "Bot stopped"
    assert ev({"t": 1, "type": "start", "baskets": 61, "git_sha": "ca784ef4e96e"})["text"] == "Bot started (61 baskets, ca784ef)"
    assert ev({"t": 1, "type": "halt", "basket": "b", "reason": "x"})["text"] == "b: HALTED, x"
    assert ev({"t": 1, "type": "other"})["text"] == "other"


def test_books_changed_at_uses_the_latest_fill_without_a_positions_file(cfg, journal):
    (cfg.state_dir / "positions.json").unlink()
    assert ctx(cfg, journal).books_changed_at() == journal.latest_fill_t() / 1000
