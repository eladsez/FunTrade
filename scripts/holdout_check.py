"""
Holdout check for exported basket configs.

Re-runs every basket config exactly as exported (same weights, window, entry/exit z)
on data AFTER the export date. That data played no part in selecting the baskets,
so this is a genuine out-of-sample test. Uses compute.py's own simulator so the
results are directly comparable to the research metrics stored in the configs.

Usage:
    python scripts/holdout_check.py [--configs config/basket_configs] [--start 2026-03-30] [--end YYYY-MM-DD] [--csv out.csv]
"""
import argparse
import glob
import json
import os
import sys
from datetime import datetime, timedelta

import numpy as np
import pandas as pd
import yfinance as yf
from statsmodels.tsa.stattools import adfuller

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compute import simulate_event_loop  # noqa: E402


def load_configs(folder):
    configs = []
    for path in sorted(glob.glob(os.path.join(folder, "*.json"))):
        with open(path) as f:
            configs.append(json.load(f))
    return configs


def download_closes(tickers, start, end):
    data = yf.download(sorted(tickers), start=start, end=end, auto_adjust=True, progress=False, threads=True)["Close"]
    data.index = data.index.tz_localize(None)
    return data


def run_basket(cfg, closes, holdout_start, cost_per_trade, max_capital, size_with_prev_close=True):
    tickers = cfg["tickers"]
    weights = cfg["weights"]
    params = cfg["trading_params"]
    window = params["window_size"]

    prices = closes[tickers].dropna()
    spread = sum(np.log(prices[t]) * weights[t] for t in tickers)

    # Rolling stats use pre-holdout data only as warm-up, exactly like the live bot would.
    z = ((spread - spread.rolling(window).mean()) / spread.rolling(window).std()).dropna()
    z = z[z.index >= holdout_start]
    if len(z) < 20:
        return None

    sharpe, trades, pct_returns = simulate_event_loop(
        z_spread=z, prices=prices, weights=weights,
        entry_z=params["entry_z"], exit_z=params["exit_z"],
        cost_per_trade=cost_per_trade, max_capital=max_capital,
        size_with_prev_close=size_with_prev_close,
    )
    pct_returns.index = z.index[1:]

    cum = pct_returns.cumsum()
    holdout_spread = spread[spread.index >= holdout_start]

    return {
        "basket_id": cfg["basket_id"],
        "research_oos_sharpe": cfg["metrics"]["oos_sharpe"],
        "holdout_sharpe": round(float(sharpe), 3),
        "holdout_return_pct": round(float(cum.iloc[-1]) * 100, 2),
        "max_drawdown_pct": round(float((cum.cummax() - cum).max()) * 100, 2),
        "trades": trades,
        "holdout_adf_p": round(float(adfuller(holdout_spread)[1]), 4),
        "days": len(z),
        "_returns": pct_returns,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--configs", default="config/basket_configs")
    ap.add_argument("--start", default="2026-03-30", help="first holdout trading day (after export)")
    ap.add_argument("--end", default=datetime.now().strftime("%Y-%m-%d"))
    ap.add_argument("--cost", type=float, default=0.002, help="cost per entry/exit as fraction of capital (compute.py default)")
    ap.add_argument("--capital", type=float, default=5000.0)
    ap.add_argument("--csv", default=None)
    ap.add_argument("--fill-day-sizing", action="store_true",
                    help="size entries with the fill day's close (original research; not achievable live)")
    args = ap.parse_args()

    configs = load_configs(args.configs)
    holdout_start = pd.Timestamp(args.start)
    max_window = max(c["trading_params"]["window_size"] for c in configs)
    # Calendar-day buffer so the longest rolling window is full before the holdout starts.
    dl_start = (holdout_start - timedelta(days=int(max_window * 1.6) + 30)).strftime("%Y-%m-%d")

    tickers = {t for c in configs for t in c["tickers"]}
    print(f"[*] {len(configs)} baskets, {len(tickers)} tickers. Holdout {args.start} -> {args.end}. Downloading from {dl_start}...")
    closes = download_closes(tickers, dl_start, args.end)

    results = [r for r in (run_basket(c, closes, holdout_start, args.cost, args.capital, not args.fill_day_sizing)
                           for c in configs) if r]
    if not results:
        print("[!] No baskets had enough holdout data.")
        return

    df = pd.DataFrame([{k: v for k, v in r.items() if k != "_returns"} for r in results])
    df = df.sort_values("holdout_sharpe", ascending=False)

    pd.set_option("display.width", 200)
    pd.set_option("display.max_rows", 200)
    print("\n=== Per-basket holdout results ===")
    print(df.to_string(index=False))

    # Equal-weight portfolio of all baskets (each with its own capital slice).
    port = pd.concat([r["_returns"] for r in results], axis=1).fillna(0.0).mean(axis=1)
    port_sharpe = np.sqrt(252) * port.mean() / port.std() if port.std() > 0 else 0.0
    t_stat = port.mean() / (port.std() / np.sqrt(len(port))) if port.std() > 0 else 0.0
    traded = df[df["trades"] > 0]

    print("\n=== Summary ===")
    print(f"Baskets evaluated:            {len(df)}  (holdout days: {df['days'].max()})")
    print(f"Baskets that traded:          {len(traded)}")
    print(f"Positive holdout return:      {(traded['holdout_return_pct'] > 0).sum()} / {len(traded)} of those that traded")
    print(f"Median holdout Sharpe:        {traded['holdout_sharpe'].median():.3f}   (research OOS median: {df['research_oos_sharpe'].median():.3f})")
    print(f"Mean holdout return:          {traded['holdout_return_pct'].mean():.2f}%")
    print(f"Still cointegrated (ADF<.05): {(df['holdout_adf_p'] < 0.05).sum()} / {len(df)}")
    print(f"Equal-weight portfolio:       Sharpe {port_sharpe:.3f}, total {port.sum() * 100:.2f}%, t-stat {t_stat:.2f}")
    print("Note: baskets share symbols (e.g. HBAN), so they are not independent; treat counts as indicative.")

    if args.csv:
        df.to_csv(args.csv, index=False)
        print(f"[*] Wrote {args.csv}")


if __name__ == "__main__":
    main()
