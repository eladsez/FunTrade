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

Market data: no paid subscription is needed. The bot trades on daily dividend-adjusted
history (free over the API) and uses 15-minute delayed quotes when there is no real-time
subscription. Two things block data: being logged in to the **live** account elsewhere (TWS,
mobile app) while the paper Gateway runs (errors 10197/162), and "Read-Only API" left on
(error 321, no orders). A non-USD account with no USD balance gets its USD rate from IBKR's
FX data at startup.

Startup order (automatic): restore books from `state/` → connect and sync account,
positions, open orders and **today's executions** (fills that happened while the bot was
down are booked, duplicates ignored) → reconcile every symbol against the broker → start
the baskets. A basket whose symbols don't reconcile **halts** instead of trading.

Run it during market hours or before the open. MOC orders are accepted until ~15:50 ET;
an order sent later is rejected and the basket simply re-decides at the next close.

## Stopping

`Ctrl+C` (SIGINT/SIGTERM). Working orders are canceled, state is saved, **positions are
kept**. The next start resumes them. Nothing is flattened on shutdown. With `state/HANDOVER`
present (deploys write it), the stop leaves working orders at the broker instead, for the next
start to reattach.

## Files

| Path | What |
|---|---|
| `state/positions.json` | Per-basket positions, realized PnL, executions already booked |
| `state/baskets/<id>.json` | Basket state, pending action, in-flight orders, halt reason, and the entry that opened the position (`entry`: decision day, entry day, z; display only) |
| `state/journal.jsonl` | Append-only events for the dashboard: each basket's close z, decision and leg closes, cycles, fills, skipped entries, halts, start/stop. The bot never reads it back |
| `state/status.json` | Heartbeat for the dashboard (every minute; `running: false` on a clean stop): equity, broker connection (`broker_connected`: our socket to the Gateway; `ibkr_connected` / `ibkr_down_since`: the Gateway's own link to IBKR), exposure, limits, per-symbol net USD (`symbol_net_usd`, held + reserved) |
| `state/zhistory/<id>.json` | Each basket's z over the 365-day warm-up history (startup only), for the dashboard's chart |
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

The dashboard's System page does the same (type-to-confirm, audit-logged).

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

## Raspberry Pi deployment

Two containers (`deploy/docker-compose.yml`): `ib-gateway` (headless IB Gateway with IBC
auto-login, daily restart 23:45 New York) and `funtrade` (the bot, connecting to it over the
compose network). Everything lives in `~/funtrade` on the Pi:

| Path | What |
|---|---|
| `.env` | Paper login (`TWS_USERID`, `TWS_PASSWORD`), optional `VNC_SERVER_PASSWORD`, and `FUNTRADE_TAG` / `DASHBOARD_TAG` (managed by deploy.sh). chmod 600, never committed |
| `docker-compose.yml`, `state_report.py` | Installed by each deploy |
| `state/`, `logs/` | The bot's state and log (same as above) |
| `dashboard/` | The dashboard's audit log (`audit.jsonl`), backups of cleared halts, and every commission IBKR reported (`commissions.jsonl`; IBKR only returns recent executions) |
| `gateway/` | Gateway settings, kept across restarts |

**Deploys.** Push to `main` → GitHub builds an arm64 image (unit tests run inside the build),
pushes `ghcr.io/eladsez/funtrade:<sha>`, then the Pi's runner (`~/actions-runner-funtrade`,
label `funtrade`) runs `deploy/deploy.sh <sha>`. The restart is a **handover**: deploy.sh
writes `state/HANDOVER`, the stopping bot leaves its working MOC orders at the broker, and the
new bot reattaches them (`Reattached leg ... still working` in the job log). So it deploys
during the session too, except 15:40-16:15 New York (MOC cutoff, auction, fills). It waits
until 16:15 when the running bot can't hand over (status.json has no `"handover": true`; any
version before this one) or the compose file changed (it may recreate the Gateway). It rolls
back if the bot doesn't log `Running N baskets` within 5 minutes, and fails the job if more
baskets are halted after the restart. "Run workflow" with *force* deploys immediately. The dashboard image (`ghcr.io/eladsez/funtrade-dashboard:<sha>`) is built on
every push; when a push changes nothing outside `dashboard/` and docs, only the dashboard is
deployed, right away, without touching the bot (`DASHBOARD_ONLY=1`).

**On the Pi:**

```bash
cd ~/funtrade
docker compose ps                         # both Up
docker compose logs -f --tail 100 funtrade
python3 state_report.py --state state     # daily check (exit 1 = a basket is halted)
grep -E "\[(warning|error)\]" logs/funtrade.log | tail
touch state/KILL                          # stop new entries (state/FLATTEN: exit all)
docker compose restart funtrade           # outside market hours
```

View the Gateway: set `VNC_SERVER_PASSWORD`, `docker compose up -d`, then from your machine
`ssh -L 5900:localhost:5900 eladsez@rpi.tail4f4240.ts.net` and open a VNC viewer on
`localhost:5900`.

**Gateway cut off from IBKR (error 1100).** Our socket stays up, but no orders or prices go
through. Short ones are routine around IBKR's midnight server reset and recover by themselves
(1101/1102; the bot logs one warning and one line when it's back). Past 5 minutes the bot logs
an error (again every 30 min), and the dashboard shows a red "Gateway cut off from IBKR"
banner. If it doesn't recover, look at the Gateway's log: on 2026-10-21 a re-login at 00:17 ET
failed with "Unrecognized Username or Password" and the Gateway sat on that dialog until
09:40. Restarting it fixed it (the credentials were fine), and the bot then sent the day's
entries by itself:

```bash
docker compose logs --since 12h ib-gateway | grep -E "IBC: (Login|detected dialog)" | tail
docker compose restart ib-gateway         # safe while no MOC orders are working
```

Restarting the Gateway cancels working MOC orders, so during the session check the Orders
page first. If the login fails again, the paper credentials need checking (IBKR portal, then
`.env`). Our own socket dropping (the nightly 23:45 restart) logs one line down and one
"Reconnected to the Gateway after N s"; past 3 minutes it's an error.

Only one Gateway may be logged in with the paper user. Close the one on WSL/desktop while the
Pi runs (the Pi's takes over: `EXISTING_SESSION_DETECTED_ACTION=primary`). Logging in to the
**live** account elsewhere still blocks market data (10197).

## Dashboard

`dashboard/` (FastAPI + React, one container, `127.0.0.1:8090` on the Pi). Pages: Overview,
Baskets, basket detail (z history, legs, trades), Orders (bot vs broker), System (controls,
halts, health, warnings).

- **Reads:** `state/` (baskets, books, journal, status), `logs/funtrade.log`, the bot's UI
  WebSocket (`funtrade:9000`, live prices and intraday z), and the Gateway through its own
  read-only connection (client id 17: open orders, positions, today's executions, net
  liquidation, every 60 s). That connection refuses order calls and market data in code.
- **Writes:** `state/KILL`, `state/FLATTEN`, and "Clear halt" (only while the bot is
  stopped: `running: false` in status.json, or no heartbeat for 5 minutes). Clearing backs
  up the basket file and `positions.json` to `dashboard/backups/` first. Every write goes to
  `dashboard/audit.jsonl` with the tailnet user when known.
- **Access:** tailnet only. One-time on the Pi (Kupa already uses 443):
  `sudo tailscale serve --bg --https=8443 http://127.0.0.1:8090`, then
  `https://rpi.tail4f4240.ts.net:8443` (add it to the phone's home screen).
- **History** (z chart, trades) starts with the first bot version that writes the journal;
  the z chart reaches back a year through `state/zhistory`.
- **Numbers to know:** open P&L uses the day's close once the bot has journaled it (after its
  18:00 ET fetch), otherwise IBKR *delayed* quotes (~15 min behind in session); each leg says
  which. "Held" counts NYSE sessions. A books-vs-broker difference is "rechecking" until two
  broker snapshots taken after the books last changed agree on it, then a mismatch. Fills
  are compared with the day's official close (what the research assumes; positive bps =
  worse). Commissions come from IBKR's commission reports. Warnings are grouped by bot run
  with repeats folded; earlier runs are collapsed.
- **Market calendar:** `dashboard/backend/app/nyse.py` (holidays and 13:00 early closes,
  2026–2027 from NYSE's published calendar). Add each new year before it starts; outside
  the known years the dashboard says so and assumes weekdays.
- **Restart only the dashboard:** `docker compose restart dashboard` (any time; the bot is
  unaffected).
- **Develop:** `dashboard/dev.sh up` (everything in containers, sample data, hot reload on
  `localhost:5173`); `dashboard/dev.sh test` runs what the image build runs.
