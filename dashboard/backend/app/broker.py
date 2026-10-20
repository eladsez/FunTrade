"""The broker's own view (IBKR open orders, positions, today's executions, net liquidation)
over a separate, read-only API connection with its own client id. It only asks; it can't
place, modify or cancel anything, and it requests no market data (the bot uses ~90 of the
100 market data lines)."""
import logging
import threading
import time

from ibapi.client import EClient
from ibapi.execution import ExecutionFilter
from ibapi.wrapper import EWrapper

log = logging.getLogger("dashboard.broker")

# EClient calls this connection must never make: they trade, or use market data lines.
FORBIDDEN = (
    "placeOrder", "cancelOrder", "reqGlobalCancel", "exerciseOptions", "reqAutoOpenOrders",
    "reqMktData", "reqMktDepth", "reqRealTimeBars", "reqTickByTickData", "reqHistoricalData",
)

# Informational codes TWS sends on every connect (market data farm OK, etc.).
INFO_CODES = {2104, 2106, 2107, 2108, 2158, 2119}


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
        self.last_error: str | None = None

    # --- connection ---
    def nextValidId(self, orderId):
        self.ready.set()

    def connectionClosed(self):
        self.ready.clear()

    def error(self, reqId, errorCode, errorString, *rest):
        if errorCode in INFO_CODES:
            return
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

    def __init__(self, host: str, port: int, client_id: int, interval: float):
        self.host, self.port, self.client_id, self.interval = host, port, client_id, interval
        self.client: ReadOnlyClient | None = None
        self.snapshot: dict = {"connected": False, "checked_at": None}
        self.version = 0
        self._stop = threading.Event()
        self._req = 9000

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
            raise TimeoutError("broker did not answer within 15 s")
        with c.lock:
            self.snapshot = {
                "connected": True, "checked_at": time.time(), "error": None,
                "orders": list(c.orders), "positions": list(c.positions), "executions": list(c.execs),
                "account": dict(c.account),
            }
        self.version += 1

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
        if self.snapshot.get("connected") or self.snapshot.get("error") != why:
            self.snapshot = {**self.snapshot, "connected": False, "error": why}
            self.version += 1
        log.info("broker unavailable: %s", why)
