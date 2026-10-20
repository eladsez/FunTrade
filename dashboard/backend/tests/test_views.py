import json
import time

from app import views


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
    from app import store
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
    assert o["unrealized"] is None, "no live prices: unrealized is unknown, not zero"


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
    assert [m["symbol"] for m in o["position_mismatches"]] == ["ED"]
    assert o["position_mismatches"][0]["broker"] == 0.0


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
    from app import store
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
