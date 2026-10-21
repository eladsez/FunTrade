"""FunTrade dashboard: REST + Server-Sent Events over the bot's files, its live feed and a
read-only broker connection, plus the built React app. One process, uvicorn on :8080."""
import asyncio
import contextlib
import dataclasses
import json
import logging
import time
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import controls, logs, nyse, settings as settings_mod, store, views
from .live import BotLive

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
log = logging.getLogger("dashboard")
logging.getLogger("ibapi").setLevel(logging.WARNING)  # it logs every message at INFO


class App:
    """Process-wide state: the sources and a version per kind of change (SSE pushes on moves)."""

    def __init__(self, s: settings_mod.Settings):
        self.s = s
        self.configs = store.load_configs(s.config_dir)
        self.journal = store.JournalIndex(s.state_dir / "journal.jsonl")
        self.live = BotLive(s.bot_ws_url)
        self.broker = None
        if s.broker_enabled:
            from .broker import BrokerView  # ibapi only when used
            self.broker = BrokerView(s.ibkr_host, s.ibkr_port, s.ibkr_client_id, s.broker_interval_s, s.data_dir)
        self.state_version = 0
        self._sig = None

    def ctx(self) -> views.Ctx:
        return views.Ctx(self.s, self.configs, self.journal, self.live, self.broker)

    def _signature(self):
        d = self.s.state_dir
        paths = [d / "positions.json", d / "status.json", d / "KILL", d / "FLATTEN", *sorted((d / "baskets").glob("*.json"))]
        sig = []
        for p in paths:
            with contextlib.suppress(FileNotFoundError):
                st = p.stat()
                sig.append((p.name, st.st_mtime_ns, st.st_size))
        return tuple(sig)

    async def watch(self):
        """Polls the state files and the journal (inotify doesn't cross every bind mount)."""
        while True:
            changed = self.journal.refresh()
            sig = self._signature()
            if changed or sig != self._sig:
                self._sig = sig
                self.state_version += 1
            await asyncio.sleep(2)


def create_app(s: settings_mod.Settings | None = None) -> FastAPI:
    s = s or settings_mod.load()
    state = App(s)
    tasks = []

    @contextlib.asynccontextmanager
    async def lifespan(_):
        state.journal.refresh()
        tasks.append(asyncio.create_task(state.watch()))
        tasks.append(asyncio.create_task(state.live.run()))
        if state.broker:
            state.broker.start()
        yield
        for t in tasks:
            t.cancel()
        if state.broker:
            state.broker.stop()

    app = FastAPI(title="FunTrade dashboard", lifespan=lifespan, docs_url=None, redoc_url=None)
    app.state.app = state

    @app.middleware("http")
    async def same_origin_writes(request: Request, call_next):
        # Writes need the app's own header (a cross-site form can't set it) and, when the
        # browser sends one, an Origin matching the Host it asked for.
        if request.method not in ("GET", "HEAD", "OPTIONS"):
            origin = request.headers.get("origin")
            host = request.headers.get("host", "")
            if request.headers.get("x-funtrade") != "1" or (origin and origin.split("://", 1)[-1] != host):
                return JSONResponse({"detail": "cross-origin write refused"}, status_code=403)
        return await call_next(request)

    def who(request: Request) -> dict:
        h = request.headers
        return {"user": h.get("tailscale-user-login"), "ip": h.get("x-forwarded-for") or (request.client.host if request.client else None),
                "agent": h.get("user-agent", "")[:160]}

    @app.get("/api/health")
    def health():
        return {"ok": True, "baskets": len(state.configs)}

    @app.get("/api/overview")
    def overview():
        return state.ctx().overview()

    @app.get("/api/baskets/{basket}")
    def basket(basket: str):
        if basket not in state.configs:
            raise HTTPException(404, "unknown basket")
        return state.ctx().detail(basket)

    @app.get("/api/orders")
    def orders():
        return state.ctx().orders()

    @app.get("/api/system")
    def system():
        return state.ctx().system(logs.problems(s.logs_dir / "funtrade.log", state.journal.start_times()))

    @app.get("/api/session")
    def session():
        return dataclasses.asdict(nyse.session())

    def live_payload():
        return {"connected": state.live.connected, "updated_at": state.live.updated_at,
                "baskets": {b: {"z": v.get("z")} for b, v in state.live.baskets.items()},
                "prices": state.live.prices()}

    @app.get("/api/live")
    def live():
        return live_payload()

    class Flag(BaseModel):
        on: bool
        confirm: str

    @app.post("/api/controls/{flag}")
    def set_flag(flag: str, body: Flag, request: Request):
        try:
            on = controls.set_flag(s.state_dir, s.data_dir, flag, body.on, body.confirm, who(request))
        except controls.ControlError as e:
            raise HTTPException(400, str(e))
        state.state_version += 1
        return {"flag": flag, "on": on}

    class ClearHalt(BaseModel):
        confirm: str
        remove_books: bool = True

    @app.post("/api/baskets/{basket}/clear-halt")
    def clear_halt(basket: str, body: ClearHalt, request: Request):
        if basket not in state.configs:
            raise HTTPException(404, "unknown basket")
        try:
            out = controls.clear_halt(s.state_dir, s.data_dir, basket, body.confirm, body.remove_books, who(request))
        except controls.ControlError as e:
            raise HTTPException(409, str(e))
        state.state_version += 1
        return out

    @app.get("/api/events")
    async def events(request: Request):
        async def stream():
            seen = {"state": state.state_version, "broker": state.broker.version if state.broker else 0,
                    "live": state.live.version}
            last_live = last_beat = 0.0
            yield "retry: 3000\n\n"
            while not await request.is_disconnected():
                now = time.monotonic()
                if state.state_version != seen["state"]:
                    seen["state"] = state.state_version
                    yield "event: state\ndata: {}\n\n"
                bv = state.broker.version if state.broker else 0
                if bv != seen["broker"]:
                    seen["broker"] = bv
                    yield "event: broker\ndata: {}\n\n"
                if state.live.version != seen["live"] and now - last_live >= 2:
                    seen["live"], last_live = state.live.version, now
                    yield f"event: live\ndata: {json.dumps(live_payload())}\n\n"
                if now - last_beat >= 15:
                    last_beat = now
                    yield ": beat\n\n"
                await asyncio.sleep(1)

        return StreamingResponse(stream(), media_type="text/event-stream",
                                 headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

    static = Path(s.static_dir)
    if (static / "index.html").exists():
        app.mount("/assets", StaticFiles(directory=static / "assets"), name="assets")

        @app.get("/{path:path}")
        def spa(path: str):
            if path.startswith("api/"):
                raise HTTPException(404)
            f = (static / path).resolve()
            if path and f.is_file() and static.resolve() in f.parents:
                return FileResponse(f)
            return FileResponse(static / "index.html", headers={"Cache-Control": "no-cache"})

    return app

