"""The broker's own view (IBKR open orders, positions, today's executions, net liquidation)
over a separate, read-only API connection with its own client id. It only asks; it can't
place, modify or cancel anything, and it requests no market data (the bot uses ~90 of the
100 market data lines).

Commissions arrive with the executions (commissionReport). IBKR only returns recent
executions, so each one is appended to <data_dir>/commissions.jsonl and kept."""
import json
import logging
import threading
import time
from pathlib import Path

from ibapi.client import EClient
from ibapi.execution import ExecutionFilter
from ibapi.wrapper import EWrapper

from .store import load_commissions

log = logging.getLogger("dashboard.broker")

# EClient calls this connection must never make: they trade, or use market data lines.
FORBIDDEN = (
    "placeOrder", "cancelOrder", "reqGlobalCancel", "exerciseOptions", "reqAutoOpenOrders",
    "reqMktData", "reqMktDepth", "reqRealTimeBars", "reqTickByTickData", "reqHistoricalData",
)

# Informational codes TWS sends on every connect (market data farm OK, etc.).
INFO_CODES = {2104, 2106, 2107, 2108, 2158, 2119}
UNSET = 1e300   # ibapi's UNSET_DOUBLE (sys.float_info.max): no value


class ReadOnlyClient(EWrapper, EClient):
    def __init__(self):
        EWrapper.__init__(self)
        EClient.__init__(self, wrapper=self)
        self.ready = threading.Event()
        self.lock = threading.Lock()
        self.done: dict[str, threading.Event] = {k: threading.Event() for k in ("orders", "positions", "execs", "account")}
        self._orders, self._positions, self._execs = {}, [], {}
        self.orders, self.positions, self.execs = [], [], []
        self.account: dict[str, dict] = {}
        self.commissions: dict[str, dict] = {}   # exec_id -> {commission, currency}
        self.last_error: str | None = None
        # Error 1100: the Gateway lost its own link to IBKR. Our socket stays up but requests
        # go unanswered until 1101/1102, or a new session after the Gateway restarts.
        self.ibkr_down_since: float | None = None

    # --- connection ---
    def nextValidId(self, orderId):
        self.ibkr_down_since = None   # the Gateway only accepts a session once logged in
        self.ready.set()

    def connectionClosed(self):
        self.ready.clear()

    def error(self, reqId, errorCode, errorString, *rest):
        if errorCode in INFO_CODES:
            return
        if errorCode == 1100:
            self.ibkr_down_since = self.ibkr_down_since or time.time()
        elif errorCode in (1101, 1102):
            self.ibkr_down_since = None
        self.last_error = f"{errorCode}: {errorString}"
        log.info("ibkr %s %s", errorCode, errorString)

    # --- open orders (all clients) ---
    def openOrder(self, orderId, contract, order, orderState):
        with self.lock:
            self._orders[order.permId or orderId] = {
                "perm_id": order.permId, "order_id": orderId, "client_id": order.clientId,
                "symbol": contract.symbol, "side": order.action, "quantity": float(order.totalQuantity),
                "type": order.orderType, "tif": order.tif, "status": orderState.status,
                "order_ref": order.orderRef or "",
            }

    def orderStatus(self, orderId, status, filled, remaining, avgFillPrice, permId, *rest):
        with self.lock:
            o = self._orders.get(permId)
            if o:
                o["status"] = status
                o["filled"] = float(filled)

    def openOrderEnd(self):
        with self.lock:
            self.orders = list(self._orders.values())
        self.done["orders"].set()

    # --- positions ---
    def position(self, account, contract, position, avgCost):
        with self.lock:
            self._positions.append({"symbol": contract.symbol, "quantity": float(position),
                                    "avg_cost": avgCost, "sec_type": contract.secType})

    def positionEnd(self):
        with self.lock:
            self.positions = [p for p in self._positions if p["quantity"] != 0]
        self.done["positions"].set()

    # --- executions (today's, all clients) ---
    def execDetails(self, reqId, contract, execution):
        with self.lock:
            self._execs[execution.execId] = {
                "exec_id": execution.execId, "symbol": contract.symbol, "side": execution.side,
                "quantity": float(execution.shares), "price": execution.price, "time": execution.time,
                "order_ref": execution.orderRef or "", "perm_id": execution.permId, "client_id": execution.clientId,
            }

    def commissionReport(self, commissionReport):
        r = commissionReport
        if r.commission is None or r.commission >= UNSET:
            return
        with self.lock:
            self.commissions[r.execId] = {"commission": float(r.commission), "currency": r.currency}

    def execDetailsEnd(self, reqId):
        with self.lock:
            self.execs = sorted(self._execs.values(), key=lambda e: e["time"])
        self.done["execs"].set()

    # --- account ---
    def accountSummary(self, reqId, account, tag, value, currency):
        with self.lock:
            try:
                self.account[tag] = {"value": float(value), "currency": currency}
            except ValueError:
                pass

    def accountSummaryEnd(self, reqId):
        self.done["account"].set()


def _forbid(name):
    def refuse(self, *a, **kw):
        raise PermissionError(f"read-only broker connection: {name} is not allowed")
    return refuse


for _name in FORBIDDEN:
    setattr(ReadOnlyClient, _name, _forbid(_name))


class BrokerView:
    """Polls the broker every `interval` seconds on its own thread; reconnects after the
    Gateway's daily restart."""

    def __init__(self, host: str, port: int, client_id: int, interval: float, data_dir: Path | None = None):
        self.host, self.port, self.client_id, self.interval = host, port, client_id, interval
        self.client: ReadOnlyClient | None = None
        self.snapshot: dict = {"connected": False, "checked_at": None}
        self.previous: dict | None = None   # the snapshot before, to tell settling from mismatched
        self.version = 0
        self._stop = threading.Event()
        self._req = 9000
        self._commission_file = data_dir / "commissions.jsonl" if data_dir else None
        self.commissions: dict[str, dict] = load_commissions(self._commission_file) if data_dir else {}

    def start(self):
        threading.Thread(target=self._loop, name="broker", daemon=True).start()

    def stop(self):
        self._stop.set()
        if self.client:
            self.client.disconnect()

    def _connect(self) -> bool:
        if self.client and self.client.isConnected() and self.client.ready.is_set():
            return True
        if self.client:
            self.client.disconnect()
        c = ReadOnlyClient()
        c.connect(self.host, self.port, self.client_id)
        if not c.isConnected():
            return False
        threading.Thread(target=c.run, name="broker-reader", daemon=True).start()
        if not c.ready.wait(10):
            c.disconnect()
            return False
        c.reqAccountSummary(self._next(), "All", "NetLiquidation,TotalCashValue,ExchangeRate")
        self.client = c
        log.info("connected to %s:%s as client %s", self.host, self.port, self.client_id)
        return True

    def _next(self) -> int:
        self._req += 1
        return self._req

    def poll_once(self):
        c = self.client
        for ev in ("orders", "positions", "execs"):
            c.done[ev].clear()
        with c.lock:
            c._orders, c._positions, c._execs = {}, [], {}
        c.reqAllOpenOrders()
        c.reqPositions()
        c.reqExecutions(self._next(), ExecutionFilter())
        ok = all(c.done[ev].wait(15) for ev in ("orders", "positions", "execs"))
        c.cancelPositions()
        if not ok:
            raise TimeoutError("Gateway is cut off from IBKR (1100)" if c.ibkr_down_since
                               else "broker did not answer within 15 s")
        with c.lock:
            snap = {
                "connected": True, "checked_at": time.time(), "error": None, "ibkr_down_since": c.ibkr_down_since,
                "orders": list(c.orders), "positions": list(c.positions), "executions": list(c.execs),
                "account": dict(c.account),
            }
            execs = {e["exec_id"]: e for e in snap["executions"]}
            # A report can arrive after this poll's execution list; it's taken on the next one.
            fresh = {k: v for k, v in c.commissions.items() if k not in self.commissions and k in execs}
        self.record_commissions(fresh, execs)
        if self.snapshot.get("checked_at"):
            self.previous = self.snapshot
        self.snapshot = snap
        self.version += 1

    def record_commissions(self, fresh: dict[str, dict], execs: dict[str, dict]):
        """Keeps new commissions (with the execution's symbol, orderRef and time, so they can
        be attributed after IBKR stops returning the execution)."""
        rows = []
        for exec_id, c in fresh.items():
            e = execs[exec_id]
            row = {"exec_id": exec_id, **c, "symbol": e["symbol"], "order_ref": e.get("order_ref", ""),
                   "time": e.get("time")}
            self.commissions[exec_id] = row
            rows.append(row)
        if rows and self._commission_file:
            try:
                self._commission_file.parent.mkdir(parents=True, exist_ok=True)
                with self._commission_file.open("a") as f:
                    f.writelines(json.dumps(r) + "\n" for r in rows)
            except OSError as e:
                log.warning("cannot save commissions: %s", e)

    def _loop(self):
        while not self._stop.is_set():
            try:
                if self._connect():
                    self.poll_once()
                else:
                    self._down("cannot connect to the Gateway")
            except Exception as e:  # Gateway restarting, timeouts: report and retry
                self._down(str(e))
            self._stop.wait(self.interval)

    def _down(self, why: str):
        since = self.client.ibkr_down_since if self.client and self.client.isConnected() else None
        if self.snapshot.get("connected") or self.snapshot.get("error") != why or self.snapshot.get("ibkr_down_since") != since:
            self.snapshot = {**self.snapshot, "connected": False, "error": why, "ibkr_down_since": since}
            self.version += 1
        log.info("broker unavailable: %s", why)
