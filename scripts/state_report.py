"""
Daily state report for a live/paper run: what every basket is doing, what the bot holds,
and anything that needs a human. Reads the bot's state files; never touches the broker.

Usage:
    python scripts/state_report.py [--state state]
Exit code 1 if any basket is halted (handy for cron/alerting).
"""
import argparse
import glob
import json
import os
from collections import defaultdict
from datetime import date, timedelta


def day_str(d):
    return (date(1970, 1, 1) + timedelta(days=int(d))).isoformat() if d else "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", default="state")
    args = ap.parse_args()

    pos_file = os.path.join(args.state, "positions.json")
    positions = json.load(open(pos_file))["positions"] if os.path.exists(pos_file) else {}
    by_basket = defaultdict(dict)
    for key, p in positions.items():
        basket, _, sym = key.rpartition("_")
        by_basket[basket][sym] = p

    baskets = []
    for path in sorted(glob.glob(os.path.join(args.state, "baskets", "*.json"))):
        baskets.append(json.load(open(path)))

    halted, active = [], 0
    print(f"{'basket':18s} {'state':17s} {'side':6s} {'pending':12s} {'decided':10s} {'PnL':>9s}  legs")
    for b in baskets:
        legs = by_basket.get(b["basket_id"], {})
        pnl = sum(p.get("realized_pnl", 0.0) for p in legs.values())
        held = ", ".join(f"{s} {p['quantity']:+g}" for s, p in legs.items() if abs(p["quantity"]) > 1e-9) or "flat"
        print(f"{b['basket_id']:18s} {b['master_state']:17s} {b['position']:6s} {b['pending_action']:12s} "
              f"{day_str(b.get('decision_day')):10s} {pnl:9.2f}  {held}")
        if b["master_state"] == "SYSTEM_HALTED":
            halted.append(b)
        if b["master_state"] != "FLAT":
            active += 1

    total_realized = sum(p.get("realized_pnl", 0.0) for p in positions.values())
    print(f"\n{len(baskets)} baskets, {active} not flat, {len(halted)} halted. Realized PnL (all time): {total_realized:.2f}")
    for flag in ("KILL", "FLATTEN"):
        if os.path.exists(os.path.join(args.state, flag)):
            print(f"!! {flag} file is present: {'no new entries' if flag == 'KILL' else 'all baskets exiting'}")
    for b in halted:
        print(f"!! HALTED {b['basket_id']}: {b.get('halt_reason') or '(no reason recorded)'}")
        print(f"   Fix the cause, then delete {os.path.join(args.state, 'baskets', b['basket_id'] + '.json')} "
              f"(and its legs in positions.json if they were closed manually).")
    raise SystemExit(1 if halted else 0)


if __name__ == "__main__":
    main()
