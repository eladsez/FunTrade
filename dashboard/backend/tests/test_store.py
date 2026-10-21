import json

from app import store


def lines(path, *events):
    with path.open("a") as f:
        for e in events:
            f.write(json.dumps(e) + "\n")


def test_reads_only_appended_bytes_and_waits_for_partial_lines(tmp_path):
    p = tmp_path / "journal.jsonl"
    j = store.JournalIndex(p)
    assert j.refresh() is False  # no file yet
    lines(p, {"t": 1, "type": "close", "basket": "b", "day": 10, "z": 1.0, "action": "NONE"})
    assert j.refresh() is True
    with p.open("a") as f:
        f.write('{"t": 2, "type": "close", "basket": "b", "day": 11, "z"')  # a write in progress
    j.refresh()
    assert [c["day"] for c in j.closes["b"]] == [10]
    with p.open("a") as f:
        f.write(': 2.5, "action": "ENTER_SHORT"}\n')
    j.refresh()
    assert [(c["day"], c["z"]) for c in j.closes["b"]] == [(10, 1.0), (11, 2.5)]
    assert j.refresh() is False


def test_same_day_recorded_again_after_restart_keeps_the_latest(tmp_path):
    p = tmp_path / "journal.jsonl"
    lines(p, {"t": 1, "type": "close", "basket": "b", "day": 10, "z": 1.0, "action": "NONE"},
          {"t": 2, "type": "close", "basket": "b", "day": 10, "z": 1.1, "action": "NONE"})
    j = store.JournalIndex(p)
    j.refresh()
    assert [c["z"] for c in j.closes["b"]] == [1.1]


def test_replaced_file_starts_over(tmp_path):
    p = tmp_path / "journal.jsonl"
    lines(p, *[{"t": i, "type": "close", "basket": "b", "day": i, "z": 0.0} for i in range(1, 5)])
    j = store.JournalIndex(p)
    j.refresh()
    p.write_text(json.dumps({"t": 9, "type": "close", "basket": "c", "day": 9, "z": 0.0}) + "\n")
    j.refresh()
    assert "b" not in j.closes and len(j.closes["c"]) == 1


def test_fills_attach_to_their_cycle(tmp_path):
    p = tmp_path / "journal.jsonl"
    lines(p,
          {"t": 1, "type": "cycle_start", "basket": "b", "label": "ENTRY", "day": 5,
           "legs": [{"symbol": "A", "client_order_id": "b_A#1-1"}, {"symbol": "B", "client_order_id": "b_B#1-2"}]},
          {"t": 2, "type": "fill", "basket": "b", "symbol": "A", "client_order_id": "b_A#1-1", "quantity": 3},
          {"t": 3, "type": "fill", "basket": "b", "symbol": "X", "client_order_id": "other#9-9", "quantity": 1},
          {"t": 4, "type": "cycle_end", "basket": "b", "state": "IN_POSITION", "filled": True})
    j = store.JournalIndex(p)
    j.refresh()
    c = j.snapshot_for("b")["cycles"][0]
    assert [f["symbol"] for f in c["fills"]] == ["A"]
    assert c["end"]["state"] == "IN_POSITION"
    assert j.open_cycle("b") is None
    assert j.last_entry("b")["start"]["day"] == 5


def test_corrupt_line_is_skipped(tmp_path):
    p = tmp_path / "journal.jsonl"
    p.write_text('not json\n{"t": 1, "type": "halt", "basket": "b", "reason": "x"}\n')
    j = store.JournalIndex(p)
    j.refresh()
    assert j.halts["b"][0]["reason"] == "x"


def test_day_iso_matches_the_bots_day_numbers():
    assert store.day_iso(20000) == "2024-10-04"  # tests/fakes/StrategyHarness.hpp kLastHistoryDay
    assert store.day_iso(0) is None


def test_sample_configs_load(cfg):
    c = store.load_configs(cfg.config_dir)
    assert c["hban_schw_trv"]["tickers"] == ["HBAN", "SCHW", "TRV"]
    assert c["hban_schw_trv"]["entry_z"] == 2.0


def test_closes_keep_each_legs_close_and_runs_and_fills_are_indexed(tmp_path):
    p = tmp_path / "journal.jsonl"
    lines(p, {"t": 1, "type": "start"}, {"t": 2, "type": "stop"}, {"t": 3, "type": "start"},
          {"t": 4, "type": "close", "basket": "b", "day": 10, "z": 1.0, "action": "NONE", "closes": {"A": 12.5}},
          {"t": 5, "type": "fill", "basket": "b", "symbol": "A", "exec_id": "e"})
    j = store.JournalIndex(p)
    j.refresh()
    assert j.last_close("b")["closes"] == {"A": 12.5}
    assert j.start_times() == [1, 3]
    assert j.latest_fill_t() == 5 and len(j.all_fills()) == 1
    assert store.JournalIndex(tmp_path / "none").latest_fill_t() is None


def test_load_commissions(tmp_path):
    p = tmp_path / "commissions.jsonl"
    assert store.load_commissions(p) == {}
    p.write_text('{"exec_id": "a", "commission": 1.0, "currency": "USD"}\n{"exec_id": "b", "comm')
    assert list(store.load_commissions(p)) == ["a"], "a torn last line is skipped"
