"""Client of the bot's UI WebSocket (UiBroadcaster, :9000): latest price per leg and the
intraday z per basket. Display only: the bot trades on daily closes, never on these."""
import asyncio
import json
import logging
import time

import websockets

log = logging.getLogger("dashboard.live")


class BotLive:
    def __init__(self, url: str):
        self.url = url
        self.connected = False
        self.baskets: dict[str, dict] = {}   # id -> {"z": float, "legs": {sym: {"px": float, "t": epoch s}}}
        self.updated_at: float | None = None
        self.version = 0                      # bumps on every change; SSE pushes when it moves

    def apply(self, msg: dict):
        kind, b = msg.get("type"), msg.get("basket")
        if not b:
            return
        now = time.time()
        if kind == "init_basket":
            # The bot seeds these with the last close at startup; only later ticks are quotes.
            self.baskets[b] = {"z": msg.get("z"), "legs": {s: {"px": v.get("px") or None, "t": None}
                                                          for s, v in msg.get("legs", {}).items()}}
        elif kind == "z_tick":
            self.baskets.setdefault(b, {"z": None, "legs": {}})["z"] = msg.get("z")
        elif kind == "price":
            self.baskets.setdefault(b, {"z": None, "legs": {}})["legs"][msg["sym"]] = {"px": msg.get("p"), "t": now}
        else:
            return
        self.updated_at = now
        self.version += 1

    def price(self, basket: str, symbol: str) -> float | None:
        px = self.baskets.get(basket, {}).get("legs", {}).get(symbol, {}).get("px")
        return px if px and px > 0 else None

    def prices(self) -> dict[str, float]:
        """Latest price per symbol across baskets."""
        return {s: q["px"] for s, q in self.quotes().items()}

    def quotes(self) -> dict[str, dict]:
        """Latest price per symbol across baskets, with when it arrived (epoch s; None for the
        startup seed). IBKR delayed data: in session it lags the market by ~15 min."""
        out: dict[str, dict] = {}
        for b in self.baskets.values():
            for s, leg in b["legs"].items():
                if not (leg.get("px") and leg["px"] > 0):
                    continue
                cur = out.get(s)
                if cur is None or (leg.get("t") or 0) > (cur["t"] or 0):
                    out[s] = {"px": leg["px"], "t": leg.get("t")}
        return out

    async def run(self):
        if not self.url:
            return
        backoff = 1
        while True:
            try:
                async with websockets.connect(self.url, open_timeout=5, ping_interval=20, max_size=2**22) as ws:
                    self.connected = True
                    self.version += 1
                    backoff = 1
                    log.info("connected to %s", self.url)
                    async for raw in ws:
                        try:
                            self.apply(json.loads(raw))
                        except (ValueError, KeyError, TypeError):
                            continue
            except asyncio.CancelledError:
                raise
            except Exception as e:  # bot restarting, not up yet, network: retry
                log.debug("bot ws: %s", e)
            if self.connected:
                self.connected = False
                self.version += 1
                log.info("disconnected from %s", self.url)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)
