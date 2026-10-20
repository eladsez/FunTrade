from app.live import BotLive


def test_applies_the_bots_ui_messages():
    # Shapes from include/risk/UiBroadcaster.hpp
    live = BotLive("")
    live.apply({"type": "init_basket", "basket": "b1", "master_state": "FLAT", "pos": "FLAT", "z": 0.0,
                "legs": {"A": {"px": 0.0, "wgt": 1.0}, "B": {"px": 12.5, "wgt": -1.0}}})
    live.apply({"type": "z_tick", "basket": "b1", "z": -1.7})
    live.apply({"type": "price", "basket": "b1", "sym": "A", "p": 101.25})
    live.apply({"type": "pnl", "basket": "b1", "sym": "A", "rpnl": 1, "upnl": 2})  # ignored
    assert live.baskets["b1"]["z"] == -1.7
    assert live.price("b1", "A") == 101.25
    assert live.prices() == {"A": 101.25, "B": 12.5}
    assert live.price("b1", "missing") is None


def test_zero_price_is_no_price():
    live = BotLive("")
    live.apply({"type": "init_basket", "basket": "b1", "legs": {"A": {"px": 0.0}}})
    assert live.prices() == {}
