# Operating FunTrade (IBKR paper)

## How the bot trades

Each basket trades once per day at most, on the rule validated in research:

1. **At the close of day d** (the IBKR daily bar arrives ~18:00 New York), it computes
   z(d) from dividend-adjusted daily closes and decides: enter long/short, exit, or nothing.
2. **At the first market event of day d+1** it sends **market-on-close (MOC)** orders,
   which fill in the closing auction at the official close of d+1.

Nothing trades intraday. The intraday z on the UI is informational.

## Starting

```bash
# IB Gateway (paper) on 127.0.0.1:4002 with API enabled, "Read-Only API" off.
export IBKR_HOST=127.0.0.1 IBKR_PORT=4002 IBKR_CLIENT_ID=1   # defaults shown
./build/FunTrade
```

Startup order (automatic): restore books from `state/` → connect and sync account,
positions, open orders and **today's executions** (fills that happened while the bot was
down are booked, duplicates ignored) → reconcile every symbol against the broker → start
the baskets. A basket whose symbols don't reconcile **halts** instead of trading.

Run it during market hours or before the open. MOC orders are accepted until ~15:50 ET;
an order sent later is rejected and the basket simply re-decides at the next close.

## Stopping

`Ctrl+C` (SIGINT/SIGTERM). Working orders are canceled, state is saved, **positions are
kept**. The next start resumes them. Nothing is flattened on shutdown.

## Files

| Path | What |
|---|---|
| `state/positions.json` | Per-basket positions, realized PnL, executions already booked |
| `state/baskets/<id>.json` | Basket state, pending action, in-flight orders, halt reason |
| `logs/funtrade.log` | Rotating log (10 × 10 MB); warnings and errors flushed immediately |
| `config/risk.json` | Allocation and risk limits (fractions of account equity) |
| `config/basket_configs/` | The baskets |

State files are written atomically. **Never edit them while the bot runs.**

## Controls

| Action | How |
|---|---|
| Stop new entries (keep positions, exits still allowed) | `touch state/KILL` |
| Exit every basket at the next session | `touch state/FLATTEN` |
| Resume | delete the file |

## Daily checks

1. `python scripts/state_report.py`. Exit code 1 means a basket is halted.
2. In the log, the line `=== Daily reconciliation: ... N mismatches ===` must say **0**.
   Any `MISMATCH` line names a symbol where the bot's books and the broker disagree.
3. Errors: `grep -E "\[error\]" logs/funtrade.log`.

## Clearing a halt

A basket halts (`SYSTEM_HALTED`) when it can't be sure of its inventory: a reconciliation
mismatch at startup, a recovery sweep that failed, or the per-basket loss limit. It never
trades again on its own.

1. Read the reason: `scripts/state_report.py`, or `halt_reason` in `state/baskets/<id>.json`.
2. Fix the position at the broker (close the legs by hand if needed).
3. With the bot stopped: delete `state/baskets/<id>.json` and remove that basket's keys
   (`<id>_<SYMBOL>`) from `state/positions.json` if you closed them manually.
4. Start the bot. The basket starts FLAT.

## Periodic checks

- **Monthly:** `python scripts/holdout_check.py`. Is the edge still there on data the
  baskets were never selected on? Watch the portfolio Sharpe and t-stat trend.
- **After any strategy change:** `python scripts/parity_check.py --binary build/FunTrade --size-with-prev-close`.
  It must report every basket identical to the research simulator.

## Paper-trading exit criteria (before considering real money)

- At least 4–6 weeks with zero reconciliation mismatches and no unexplained halts.
- Live fills track the simulator: for the same days, `holdout_check.py` and the bot's
  realized PnL agree within costs.
- At least one deliberate restart mid-position and one Gateway disconnect handled correctly.
