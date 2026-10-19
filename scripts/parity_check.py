"""
Parity check: does the C++ bot trade exactly what the research simulator trades?

For each basket config it downloads dividend-adjusted daily closes (same source as
compute.py), writes a warm-up CSV and a bar-only replay CSV, runs the real C++ backtest
(`FunTrade --backtest`), replays compute.py's event loop on the same data while recording
trades, and compares the two trade lists.

A match means: same trading days, same legs, same sides, same fill prices (both fill at the
close). Share counts can differ slightly by design: the simulator sizes with the fill day's
close (unknowable when the order is sent), the bot sizes with the latest close it has.

Usage:
    python scripts/parity_check.py --binary build/FunTrade [--configs config/basket_configs]
           [--start 2026-03-30] [--end YYYY-MM-DD] [--limit N]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta

import numpy as np
import pandas as pd
import yfinance as yf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compute import calculate_execution_shares  # noqa: E402

DAY_MS = 86_400_000


def day_index(ts):
    return int(pd.Timestamp(ts).normalize().value // 1_000_000 // DAY_MS)


def research_trades(prices, cfg, start, cost_capital=5000.0, size_with_prev_close=False):
    """compute.simulate_event_loop, verbatim logic, but returning the trades it makes.

    size_with_prev_close: size entries with the previous close (what the bot knows when it
    sends the order) instead of the fill day's close (what compute.py uses)."""
    tickers, weights = cfg["tickers"], cfg["weights"]
    p = cfg["trading_params"]
    spread = sum(np.log(prices[t]) * weights[t] for t in tickers)
    w = p["window_size"]
    z = ((spread - spread.rolling(w).mean()) / spread.rolling(w).std()).dropna()
    # Include the last warm-up day: its close drives the first holdout day's action, as in the bot.
    before = z[z.index < start]
    z = pd.concat([before.iloc[-1:], z[z.index >= start]]) if len(before) else z[z.index >= start]

    trades, position, shares = [], 0, {}
    aligned = prices.loc[z.index]
    for i in range(1, len(z)):
        today_z = z.iloc[i - 1]
        today = aligned.iloc[i].to_dict()
        sizing = aligned.iloc[i - 1].to_dict() if size_with_prev_close else today
        day = day_index(z.index[i])

        def record(direction_shares, sign):
            for t, q in direction_shares.items():
                leg_dir = np.sign(weights[t]) * sign
                trades.append((day, t, "BUY" if leg_dir > 0 else "SELL", float(q), round(today[t], 4)))

        if position == 1 and today_z >= -p["exit_z"]:
            record(shares, -1); position, shares = 0, {}
        elif position == -1 and today_z <= p["exit_z"]:
            record(shares, 1); position, shares = 0, {}
        elif position == 0:
            if today_z < -p["entry_z"]:
                s = calculate_execution_shares(sizing, weights, direction=1, max_capital=cost_capital)
                if s:
                    position, shares = 1, s
                    record(s, 1)
            elif today_z > p["entry_z"]:
                s = calculate_execution_shares(sizing, weights, direction=-1, max_capital=cost_capital)
                if s:
                    position, shares = -1, s
                    record(s, -1)
    return trades


FILL_RE = re.compile(r"FILL day=(\d+) (BUY|SELL) (\S+) ([\d.]+) @ ([\d.]+)")


def cpp_trades(binary, prices, cfg_path, start, workdir):
    warm, ticks = os.path.join(workdir, "warmup.csv"), os.path.join(workdir, "ticks.csv")
    with open(warm, "w") as f:
        f.write("timestamp_ms,symbol,close,volume\n")
        for ts, row in prices[prices.index < start].iterrows():
            for sym, px in row.items():
                f.write(f"{day_index(ts) * DAY_MS},{sym},{px:.6f},0\n")
    with open(ticks, "w") as f:
        f.write("timestamp_ms,symbol,side,price,volume,trade_id\n")
        n = 0
        for ts, row in prices[prices.index >= start].iterrows():
            for sym, px in row.items():
                n += 1
                f.write(f"{day_index(ts) * DAY_MS},{sym},bar,{px:.6f},0,{n}\n")

    out = subprocess.run([binary, "--backtest", ticks, warm, cfg_path], capture_output=True, text=True, timeout=600)
    if out.returncode != 0:
        raise RuntimeError(f"C++ backtest failed ({out.returncode}): {out.stderr[-2000:]}")
    return [(int(d), sym, side, float(q), round(float(px), 4))
            for d, side, sym, q, px in FILL_RE.findall(out.stdout)]


def compare(py, cpp):
    key = lambda t: (t[0], t[1], t[2])
    pyk, cppk = {key(t): t for t in py}, {key(t): t for t in cpp}
    only_py = sorted(set(pyk) - set(cppk))
    only_cpp = sorted(set(cppk) - set(pyk))
    common = set(pyk) & set(cppk)
    price_mismatch = [k for k in common if abs(pyk[k][4] - cppk[k][4]) > 1e-3]
    qty_diff = [abs(pyk[k][3] - cppk[k][3]) / max(pyk[k][3], 1) for k in common]
    return only_py, only_cpp, price_mismatch, (max(qty_diff) if qty_diff else 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--configs", default="config/basket_configs")
    ap.add_argument("--start", default="2026-03-30")
    ap.add_argument("--end", default=datetime.now().strftime("%Y-%m-%d"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--size-with-prev-close", action="store_true",
                    help="size the simulator's entries like the live bot (previous close)")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.configs, "*.json")))
    if args.limit:
        paths = paths[: args.limit]
    cfgs = [(p, json.load(open(p))) for p in paths]
    start = pd.Timestamp(args.start)
    max_w = max(c["trading_params"]["window_size"] for _, c in cfgs)
    dl_start = (start - timedelta(days=int(max_w * 1.6) + 30)).strftime("%Y-%m-%d")
    tickers = sorted({t for _, c in cfgs for t in c["tickers"]})
    closes = yf.download(tickers, start=dl_start, end=args.end, auto_adjust=True, progress=False)["Close"]
    closes.index = closes.index.tz_localize(None)

    exact = 0
    rows = []
    for path, cfg in cfgs:
        prices = closes[cfg["tickers"]].dropna()
        if len(prices[prices.index >= start]) < 20:
            continue
        with tempfile.TemporaryDirectory() as wd:
            py = research_trades(prices, cfg, start, size_with_prev_close=args.size_with_prev_close)
            cpp = cpp_trades(os.path.abspath(args.binary), prices, os.path.abspath(path), start, wd)
        only_py, only_cpp, px_bad, qty = compare(py, cpp)
        ok = not only_py and not only_cpp and not px_bad
        exact += ok
        rows.append((cfg["basket_id"], len(py), len(cpp), len(only_py), len(only_cpp), len(px_bad), round(qty * 100, 2), ok))
        if not ok:
            print(f"[{cfg['basket_id']}] python-only: {only_py[:4]}  cpp-only: {only_cpp[:4]}  price-mismatch: {px_bad[:4]}")

    df = pd.DataFrame(rows, columns=["basket", "py_fills", "cpp_fills", "py_only", "cpp_only", "px_mismatch", "max_qty_diff_%", "match"])
    pd.set_option("display.width", 200)
    print(df.to_string(index=False))
    print(f"\n{exact}/{len(rows)} baskets: identical trade days, legs, sides and fill prices.")


if __name__ == "__main__":
    main()
