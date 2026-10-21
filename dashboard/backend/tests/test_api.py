import pytest
from fastapi.testclient import TestClient

from app.main import create_app

H = {"x-funtrade": "1"}


@pytest.fixture
def client(cfg):
    with TestClient(create_app(cfg)) as c:
        yield c


def test_reads(client):
    assert client.get("/api/health").json()["baskets"] == 61
    assert len(client.get("/api/overview").json()["baskets"]) == 61
    assert client.get("/api/baskets/fitb_key_tfc").json()["state"] == "IN_POSITION"
    assert client.get("/api/baskets/nope").status_code == 404
    assert "orders" in client.get("/api/orders").json()
    s = client.get("/api/system").json()
    assert [r["current"] for r in s["logs"]] == [True, False]
    assert any("MISMATCH IP" in l["message"] for l in s["logs"][1]["lines"]), "the earlier run's error"
    assert not any("MISMATCH IP" in l["message"] for l in s["logs"][0]["lines"])
    assert s["symbol_caps"][0]["symbol"] == "SCHW"


def test_session(client):
    s = client.get("/api/session").json()
    assert set(s) >= {"open", "day", "close_ms", "cutoff_ms", "open_ms", "eod_ms", "holiday", "early_close", "known"}
    assert s["cutoff_ms"] == s["close_ms"] - 600_000


def test_writes_need_the_apps_header(client, cfg):
    r = client.post("/api/controls/kill", json={"on": True, "confirm": "KILL"})
    assert r.status_code == 403
    assert not (cfg.state_dir / "KILL").exists()


def test_cross_origin_writes_refused(client, cfg):
    r = client.post("/api/controls/kill", json={"on": True, "confirm": "KILL"},
                    headers={**H, "origin": "https://evil.example"})
    assert r.status_code == 403
    assert not (cfg.state_dir / "KILL").exists()


def test_kill_round_trip(client, cfg):
    r = client.post("/api/controls/kill", json={"on": True, "confirm": "KILL"},
                    headers={**H, "origin": "http://testserver"})
    assert r.status_code == 200 and r.json()["on"] is True
    assert client.get("/api/overview").json()["flags"]["kill"] is True
    assert client.post("/api/controls/kill", json={"on": True, "confirm": "nope"}, headers=H).status_code == 400


def test_clear_halt_conflicts_while_running(client):
    r = client.post("/api/baskets/cms_ed_ppl/clear-halt", json={"confirm": "cms_ed_ppl"}, headers=H)
    assert r.status_code == 409
