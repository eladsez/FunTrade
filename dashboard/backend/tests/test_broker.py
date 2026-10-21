import pytest

from app.broker import FORBIDDEN, ReadOnlyClient


@pytest.mark.parametrize("name", FORBIDDEN)
def test_the_broker_connection_cannot_trade_or_take_market_data(name):
    c = ReadOnlyClient()
    with pytest.raises(PermissionError):
        getattr(c, name)(1, None, None)


def test_open_orders_and_executions_are_collected():
    c = ReadOnlyClient()

    class O:  # minimal ibapi Order / Contract / OrderState / Execution stand-ins
        pass

    contract, order, ostate = O(), O(), O()
    contract.symbol, contract.secType = "HBAN", "STK"
    order.permId, order.clientId, order.action, order.totalQuantity = 77, 1, "BUY", 241
    order.orderType, order.tif, order.orderRef = "MOC", "DAY", "hban_schw_trv_HBAN#1-1"
    ostate.status = "PreSubmitted"
    c.openOrder(5, contract, order, ostate)
    c.orderStatus(5, "Submitted", 0, 241, 0.0, 77, 0, 0.0, 1, "", 0.0)
    c.openOrderEnd()
    assert c.orders[0]["order_ref"] == "hban_schw_trv_HBAN#1-1"
    assert c.orders[0]["status"] == "Submitted"
    c.position("DU1", contract, 0, 0.0)
    c.position("DU1", contract, 10, 12.3)
    c.positionEnd()
    assert [p["quantity"] for p in c.positions] == [10.0]


class Report:
    def __init__(self, exec_id, commission, currency="USD"):
        self.execId, self.commission, self.currency = exec_id, commission, currency


def test_commission_reports_are_collected_and_unset_values_ignored():
    c = ReadOnlyClient()
    c.commissionReport(Report("e1", 1.02))
    c.commissionReport(Report("e2", 1.7976931348623157e308))
    assert c.commissions == {"e1": {"commission": 1.02, "currency": "USD"}}


def test_commissions_are_kept_across_restarts(tmp_path):
    from app.broker import BrokerView
    v = BrokerView("h", 1, 17, 60, tmp_path)
    v.record_commissions({"e1": {"commission": 1.0, "currency": "USD"}},
                         {"e1": {"exec_id": "e1", "symbol": "HBAN", "order_ref": "b_HBAN#1-1", "time": "20261020  15:59:30"}})
    again = BrokerView("h", 1, 17, 60, tmp_path)
    assert again.commissions["e1"] == {"exec_id": "e1", "commission": 1.0, "currency": "USD", "symbol": "HBAN",
                                       "order_ref": "b_HBAN#1-1", "time": "20261020  15:59:30"}
    assert BrokerView("h", 1, 17, 60).commissions == {}, "no data dir: nothing kept"


class FakeClient:
    """Answers every request at once, like a Gateway with one position and one execution."""

    def __init__(self, qty):
        import threading
        self.lock = threading.Lock()
        self.done = {k: threading.Event() for k in ("orders", "positions", "execs", "account")}
        self.orders, self.positions, self.account = [], [{"symbol": "A", "quantity": qty, "sec_type": "STK"}], {}
        self.execs = [{"exec_id": "e1", "symbol": "A", "order_ref": "b_A#1-1", "time": "t"}]
        self.commissions = {"e1": {"commission": 1.0, "currency": "USD"}, "late": {"commission": 2.0, "currency": "USD"}}
        self.ibkr_down_since = None

    def reqAllOpenOrders(self):
        self.done["orders"].set()

    def reqPositions(self):
        self.done["positions"].set()

    def reqExecutions(self, *_):
        self.done["execs"].set()

    def cancelPositions(self):
        pass


def test_poll_keeps_the_previous_snapshot_and_records_commissions(tmp_path):
    from app.broker import BrokerView
    v = BrokerView("h", 1, 17, 60, tmp_path)
    v.client = FakeClient(5)
    v.poll_once()
    assert v.previous is None and v.snapshot["positions"][0]["quantity"] == 5
    assert set(v.commissions) == {"e1"}, "a report for an execution not listed yet waits for the next poll"
    v.client = FakeClient(7)
    v.poll_once()
    assert v.previous["positions"][0]["quantity"] == 5 and v.snapshot["positions"][0]["quantity"] == 7
    assert v.version == 2
    assert len((tmp_path / "commissions.jsonl").read_text().splitlines()) == 1, "each commission written once"


def test_an_unwritable_commission_file_only_logs(tmp_path):
    from app.broker import BrokerView
    (tmp_path / "file").write_text("")
    v = BrokerView("h", 1, 17, 60, tmp_path / "file")   # its "directory" is a file
    v.record_commissions({"e1": {"commission": 1.0, "currency": "USD"}}, {"e1": {"exec_id": "e1", "symbol": "A"}})
    assert v.commissions["e1"]["commission"] == 1.0, "kept in memory regardless"


def test_error_1100_marks_the_gateway_cut_off_until_it_recovers():
    c = ReadOnlyClient()
    c.error(-1, 1100, "Connectivity between IBKR and Trader Workstation has been lost.")
    first = c.ibkr_down_since
    assert first is not None
    c.error(-1, 1100, "again")
    assert c.ibkr_down_since == first, "the outage keeps its start"
    c.error(-1, 1102, "restored, data maintained")
    assert c.ibkr_down_since is None
    c.error(-1, 1100, "lost")
    c.error(-1, 1101, "restored, data lost")
    assert c.ibkr_down_since is None
    c.error(-1, 1100, "lost")
    c.nextValidId(1)
    assert c.ibkr_down_since is None, "a new session means the Gateway logged in again"


class SilentClient(FakeClient):
    def __init__(self, down_since):
        super().__init__(0)
        self.ibkr_down_since = down_since

    def reqExecutions(self, *_):
        pass   # never answers

    def isConnected(self):
        return True


def test_an_unanswered_poll_says_why(tmp_path, monkeypatch):
    from app import broker
    monkeypatch.setattr(broker.threading.Event, "wait", lambda self, t=None: self.is_set())
    v = broker.BrokerView("h", 1, 17, 60)
    v.client = SilentClient(123.0)
    with pytest.raises(TimeoutError, match="cut off from IBKR"):
        v.poll_once()
    v._down("Gateway is cut off from IBKR (1100)")
    assert v.snapshot["ibkr_down_since"] == 123.0 and v.snapshot["connected"] is False
    version = v.version
    v._down("Gateway is cut off from IBKR (1100)")
    assert v.version == version, "an unchanged outage isn't a new snapshot"
    v.client = SilentClient(None)
    with pytest.raises(TimeoutError, match="within 15 s"):
        v.poll_once()


def test_a_good_poll_reports_the_link_up(tmp_path):
    from app.broker import BrokerView
    v = BrokerView("h", 1, 17, 60)
    v.client = FakeClient(1)
    v.poll_once()
    assert v.snapshot["ibkr_down_since"] is None
