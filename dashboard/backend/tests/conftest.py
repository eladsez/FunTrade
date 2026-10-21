import os
from dataclasses import replace
from pathlib import Path

import pytest

from app import sample, settings, store

# The image has the configs at /app/config; a checkout has them at the repo root.
CONFIG = Path(os.environ.get("FUNTRADE_CONFIG") or Path(__file__).resolve().parents[3] / "config")


@pytest.fixture
def cfg(tmp_path) -> settings.Settings:
    s = replace(settings.load(), state_dir=tmp_path / "state", logs_dir=tmp_path / "logs", config_dir=CONFIG,
                data_dir=tmp_path / "dash", static_dir=tmp_path / "static", bot_ws_url="", broker_enabled=False)
    sample.write(s.state_dir, s.logs_dir, CONFIG, data=s.data_dir)
    return s


@pytest.fixture
def journal(cfg) -> store.JournalIndex:
    j = store.JournalIndex(cfg.state_dir / "journal.jsonl")
    j.refresh()
    return j
