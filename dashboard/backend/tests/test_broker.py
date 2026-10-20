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
