"""Paths and connection settings, from the environment (compose sets them on the Pi)."""
import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Settings:
    state_dir: Path      # the bot's state/ (read; the dashboard writes only KILL, FLATTEN, halt clears)
    logs_dir: Path       # the bot's logs/ (read-only)
    config_dir: Path     # basket configs + risk.json, baked into the image from the same commit
    data_dir: Path       # the dashboard's own files: audit log, backups of cleared halts
    static_dir: Path     # built frontend
    bot_ws_url: str      # UiBroadcaster (live prices, intraday z); empty = off
    ibkr_host: str
    ibkr_port: int
    ibkr_client_id: int  # never the bot's (1)
    broker_enabled: bool
    broker_interval_s: float


def load() -> Settings:
    env = os.environ.get
    return Settings(
        state_dir=Path(env("FUNTRADE_STATE", "/data/state")),
        logs_dir=Path(env("FUNTRADE_LOGS", "/data/logs")),
        config_dir=Path(env("FUNTRADE_CONFIG", "/app/config")),
        data_dir=Path(env("DASHBOARD_DATA", "/data/dashboard")),
        static_dir=Path(env("DASHBOARD_STATIC", "/app/static")),
        bot_ws_url=env("BOT_WS_URL", "ws://funtrade:9000"),
        ibkr_host=env("IBKR_HOST", "ib-gateway"),
        ibkr_port=int(env("IBKR_PORT", "4004")),
        ibkr_client_id=int(env("IBKR_CLIENT_ID", "17")),
        broker_enabled=env("BROKER_ENABLED", "1") == "1",
        broker_interval_s=float(env("BROKER_INTERVAL_S", "60")),
    )
