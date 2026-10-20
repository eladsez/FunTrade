import json
import time

import pytest

from app import controls

WHO = {"user": "test", "ip": "127.0.0.1", "agent": "pytest"}


def stop_bot(cfg):
    p = cfg.state_dir / "status.json"
    s = json.loads(p.read_text())
    s.update(running=False, updated_at=int(time.time() * 1000))
    p.write_text(json.dumps(s))


def test_kill_needs_the_word_and_toggles_the_file(cfg):
    with pytest.raises(controls.ControlError):
        controls.set_flag(cfg.state_dir, cfg.data_dir, "kill", True, "yes", WHO)
    assert not (cfg.state_dir / "KILL").exists()
    assert controls.set_flag(cfg.state_dir, cfg.data_dir, "kill", True, "KILL", WHO) is True
    assert (cfg.state_dir / "KILL").exists()
    assert controls.set_flag(cfg.state_dir, cfg.data_dir, "kill", False, "KILL", WHO) is False
    assert not (cfg.state_dir / "KILL").exists()
    audit = [json.loads(l) for l in (cfg.data_dir / "audit.jsonl").read_text().splitlines()]
    assert [a["action"] for a in audit] == ["kill_on", "kill_off"]


def test_unknown_flag_is_refused(cfg):
    with pytest.raises(controls.ControlError):
        controls.set_flag(cfg.state_dir, cfg.data_dir, "positions", True, "POSITIONS", WHO)


def test_clear_halt_refused_while_the_bot_runs(cfg):
    with pytest.raises(controls.ControlError, match="running"):
        controls.clear_halt(cfg.state_dir, cfg.data_dir, "cms_ed_ppl", "cms_ed_ppl", True, WHO)
    assert (cfg.state_dir / "baskets" / "cms_ed_ppl.json").exists()


def test_clear_halt_refused_for_a_basket_that_is_not_halted(cfg):
    stop_bot(cfg)
    with pytest.raises(controls.ControlError, match="not halted"):
        controls.clear_halt(cfg.state_dir, cfg.data_dir, "fitb_key_tfc", "fitb_key_tfc", True, WHO)


def test_clear_halt_backs_up_then_removes_state_and_books(cfg):
    stop_bot(cfg)
    before = json.loads((cfg.state_dir / "positions.json").read_text())
    out = controls.clear_halt(cfg.state_dir, cfg.data_dir, "cms_ed_ppl", "cms_ed_ppl", True, WHO)
    assert out["removed_keys"] == ["cms_ed_ppl_ED"]
    assert not (cfg.state_dir / "baskets" / "cms_ed_ppl.json").exists()
    after = json.loads((cfg.state_dir / "positions.json").read_text())
    assert "cms_ed_ppl_ED" not in after["positions"]
    assert "fitb_key_tfc_FITB" in after["positions"], "other baskets' books untouched"
    assert after["seen_exec_ids"] == before["seen_exec_ids"], "fill dedupe must survive"
    backup = cfg.data_dir / "backups"
    (b,) = backup.iterdir()
    assert json.loads((b / "positions.json").read_text()) == before
    assert (b / "cms_ed_ppl.json").exists()


def test_clear_halt_can_keep_the_books(cfg):
    stop_bot(cfg)
    controls.clear_halt(cfg.state_dir, cfg.data_dir, "cms_ed_ppl", "cms_ed_ppl", False, WHO)
    assert "cms_ed_ppl_ED" in json.loads((cfg.state_dir / "positions.json").read_text())["positions"]
