# FunTrade

C++23 bot that trades cointegrated baskets of 2–4 US stocks (daily mean reversion) on
Interactive Brokers. Baskets come from the Python research in `scripts/compute.py`; the bot
must trade exactly what that research simulates. IBKR is the only maintained broker; the
Alpaca code still compiles but is not maintained.

Status (Oct 2026): hardened; paper trading on the Raspberry Pi since 2026-10-20 (IBKR paper
account, base currency ILS, ~$82k). Not cleared for real money: the plan is 4–6 clean paper
weeks first. See `docs/ROADMAP.md` (phases, results) and `docs/OPERATIONS.md` (runbook).

## Build and test

```bash
sudo apt install libboost-system-dev libboost-thread-dev libssl-dev libprotobuf-dev protobuf-compiler
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/tests/unit_tests          # ~110 tests, <1 s, fully offline
```

- GTest, spdlog, nlohmann_json and concurrentqueue come from FetchContent. `external/twsapi`
  is a git submodule (TWS API C++ client; needs Protobuf 3.21.x, which Ubuntu 24.04 ships).
- Always build with `-j`. Project code builds with zero warnings; keep it that way
  (warnings from `external/` are not ours).
- Live broker tests are opt-in: `-DFUNTRADE_LIVE_TESTS=ON` builds `test_ibkr_execution`.
  **Do not run it against the paper account the bot trades**: it places market and GTC
  orders (AAPL, SPY, MSFT, TSLA, NVDA) and leaves positions/orders behind, which makes the
  bot's reconciliation halt baskets.
- Sanitizers: ASan/UBSan work normally. TSan needs
  `-DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST` and running under `setarch -R`.

## Running

- Live (IBKR): `./build/FunTrade`. Env `IBKR_HOST` (127.0.0.1), `IBKR_PORT` (4002 = Gateway
  paper), `IBKR_CLIENT_ID` (1). Reads `config/basket_configs/`, `config/risk.json`; writes
  `state/` and `logs/funtrade.log` (relative to the working directory: run from repo root).
- Backtest: `./build/FunTrade --backtest <ticks.csv> <warmup.csv> <basket.json>`. Uses the
  synchronous bus and `MockExecutionEngine` (MOC orders fill at the next session's close)
  and prints `FILL day=<n> SIDE SYM QTY @ PX` lines, which `parity_check.py` parses.
- `--debug` enables debug logging.
- UI: WebSocket on port 9000 (`UiBroadcaster`), page in `index.html` (hardcoded `WS_URL`).

## Architecture

DataFeed → EventBus → Strategy → OrderManager / PortfolioManager → ExecutionEngine.

- `core/EventBus`: threaded dispatch live; synchronous mode for backtests and tests (nested
  publishes delivered inline). `post`/`flush` run work on the dispatcher thread.
  Subscribers are called in subscription order, so PortfolioManager books a fill before
  strategies react to it. Keep that order.
- `strategy/StrategyOrchestrator` loads one `BasketMeanReversionStrategy` per config.
  `BasketState` holds daily closes and computes z; `ExecutionCycle`/`LegExecutionState`
  track one entry/exit across legs; `BasketSizer` turns capital into integer shares.
- `risk/PortfolioManager`: per-basket virtual books (persisted), broker positions kept
  separately, daily reconciliation. `risk/RiskManager`: gross and per-symbol caps (with
  reservations for in-flight MOC entries), per-basket loss limit, kill/flatten files.
- `execution/IbkrExecutionEngine` + `IbkrOrderTracker` (pure logic, unit-tested) turn TWS
  callbacks into `ExecutionReport`s. `infra/IbkrInfra` owns the single `EClientSocket`.

## Invariants (breaking these caused real bugs)

Orders and fills:
- Client order id = `<basket_id>_<SYMBOL>#<epochms>-<counter>`
  (`OrderIdUtils::newClientOrderId`; `positionKeyOf` recovers the position key). IBKR
  `orderRef` carries it; executions for unknown orders with a `#` ref are restored.
- A report is a fill only when `last_quantity > 0`, and then it has an `exec_id`. Books
  dedupe by `exec_id`. On IBKR, fills come only from `execDetails`; `orderStatus` is
  status-only. Never book from cumulative filled quantities.
- Terminal states are sticky; filled quantity only grows. `PENDING_CANCEL` is not terminal.
  The tracker holds a terminal status until its executions arrive.
- OrderManager registers an order before sending it and never calls the engine while
  holding its lock. A failed placement becomes REJECTED (cycles must always resolve).
- Every TWS client call goes through `infra_->client()` (`ClientGuard`, recursive mutex).
  The client object is reused across reconnects; never free it while threads may use it.

Signal (must match `compute.py`):
- Daily closes only. z(d) = (spread(d) − mean) / sample std over the W complete days ending
  at d (pandas `rolling(W).std()`). spread = Σ weight × log(close).
- Decide at the close of day d; act at the first market event of day d+1 with
  MARKET_ON_CLOSE orders (fill at close d+1). Nothing trades intraday; the intraday z is UI only.
- Days are `tradingDay(ts) = ts / 86400000`, with daily bars stamped at UTC midnight of
  their date (`timegm`, never `mktime`).
- Daily history is IBKR `ADJUSTED_LAST` (yfinance `auto_adjust=True` equivalent). Live mode
  rebuilds the window from adjusted history at each close. A bar for today is unsettled
  until 18:00 New York (the EOD fetch).
- Entries are sized with the previous close (what is known when the order is sent).
  `compute.simulate_event_loop(size_with_prev_close=True)` is the default to match.
- No signal until the window is full (an early bug produced z≈6 from 2 days of history).

**Any change to signal, timing, or sizing must be mirrored in `compute.py` and verified with
`python scripts/parity_check.py --binary build/FunTrade --size-with-prev-close`; every basket
must report identical trades.**

Persistence and restart:
- `utils/StateStore`: atomic JSON writes (tmp, fsync, rename); corrupt files throw.
- On start: restore books → connect and sync account/positions/orders/today's executions →
  book missed fills → reconcile per symbol → baskets that mismatch HALT (never auto-fix).
  In-flight cycles are reattached. Halts persist until a human clears them.
- `stop()` cancels working orders and saves state. Shutdown never flattens.

## IBKR facts learned the hard way

- Market data: no paid subscription needed. Daily `ADJUSTED_LAST` history is free over the
  API; quotes use `reqMarketDataType(3)` (delayed when unsubscribed) and `DELAYED_*` tick
  types are mapped to live ones. Quotes only drive the UI and the session-start trigger.
- Errors 10197 / 162 "competing live session": the live account is logged in elsewhere
  (TWS, mobile). 10089: real-time subscription missing (expected; delayed is used).
  321 "Read-Only mode": Gateway API setting "Read-Only API" is on.
- The account's base currency is ILS. Values are converted with `ExchangeRate[USD]`, which
  TWS only sends while the account holds USD; otherwise main fetches `USD.ILS` MIDPOINT bars.
- 100 concurrent market data lines by default; the baskets use ~90 symbols. Adding baskets
  with new symbols can hit the limit.
- MOC orders must reach the exchange by ~15:50 ET. Later ones are rejected; the basket
  re-decides at the next close.
- Paper MOC orders show `PendingSubmit` all day (the bot sees no status change) and fill at
  ~15:59:30 ET at the last price, not at the official closing auction. Real accounts fill at
  the official close (what the research assumes), so paper P&L won't match the simulator to
  the cent. First verified 2026-10-20: 5 baskets, 15/15 legs filled, all "Entry cycle perfect".
- The Gateway restarts daily and needs a fresh login about weekly.
- `test_ibkr_execution`-style probing belongs on a separate paper account (see above).
  For read-only checks, a small Python `ibapi` script with its own client id is fine.

## Deployment (Raspberry Pi)

Paper runs on a Pi 4 (arm64, Debian 13) at `eladsez@rpi.tail4f4240.ts.net` (tailscale) in
`~/funtrade`: compose project `funtrade` = `ib-gateway` (ghcr.io/gnzsnz/ib-gateway, IBC
auto-login) + `funtrade` (ghcr.io/eladsez/funtrade:<sha>). CI: `.github/workflows/deploy.yml`
(build/test arm64 image → GHCR → self-hosted runner `funtrade` runs `deploy/deploy.sh`, which
never restarts during the US session unless forced). Runbook: `docs/OPERATIONS.md`.

Diagnosing (read-only, safe any time):
```bash
ssh eladsez@rpi.tail4f4240.ts.net 'cd ~/funtrade && docker compose ps && python3 state_report.py --state state; grep -E "\[(warning|error)\]|reconciliation|Running" logs/funtrade.log | tail -40'
```
Don't restart containers or touch `state/` during 09:00–16:15 New York without the owner's
say-so. The Pi also runs an unrelated app (`finance-app`, its own runner); leave it alone.

To see orders as IBKR sees them (the bot doesn't log PendingSubmit), run a read-only `ibapi`
script in a throwaway container on the compose network, with its own client id:
`docker run --rm --network funtrade_default -v <script>:/s.py python:3.12-slim sh -c "pip install -q ibapi==9.81.1.post1; python /s.py"`
connecting to `ib-gateway:4004` (`reqAllOpenOrders`, `reqPositions`, `reqExecutions`).
Don't log in to IBKR (web/mobile) with the paper user while the Pi runs: it takes over the
Gateway's session.

Deploy notes:
- First boot: the bot exits and Docker restarts it until the Gateway has logged in (~1 min).
  IBC accepts the paper disclaimer / API precaution dialogs (error 10141 once is normal).
- Forced deploy: Actions → build-and-deploy → Run workflow → force. Only when nothing is
  working at the broker (a restart cancels the day's MOC orders).
- `actions/checkout@v4` triggers a Node 20 deprecation warning; bump to v5 at some point.

## Open items

- `logs/funtrade.log` lags: the file sink flushes only on warnings, so recent info lines
  (fills, state changes) may be missing. `docker compose logs funtrade` is complete. Fix:
  a periodic flush (`spdlog::flush_every`), pending the owner's go-ahead.
- A React control/view dashboard (same standard as the owner's finance app) is planned.
  Ask before adding ad-hoc status tooling (extra log lines, broker-check scripts) that it
  would replace.
- Baskets are the 2026-03-28 export. Plan: re-run `compute.py` into a separate folder,
  validate on later holdout data, switch only if they hold up. Then re-select periodically.
- Concentration: ~10 baskets are HBAN/SCHW variants; the per-symbol cap admits only the
  first few in load order. Worth a smarter selection rule later.

## Research (Python)

Deps: `numpy pandas statsmodels yfinance joblib` (no requirements file; use a venv).
- `scripts/compute.py`: basket search (5y daily, 80/20 split, every 3-stock combo per sector
  in `s&p500.json`; Johansen weights, ADF/half-life/Hurst/beta filters, integer-share
  tradeability at $5,000, grid search entry/exit). Exports `config/basket_configs/*.json`.
- `scripts/holdout_check.py`: re-runs exported configs unchanged on data after export.
  The current set (exported 2026-03-28): realistic sizing → 40/60 profitable, portfolio
  Sharpe 2.50, t = 1.86. Random baskets: median 0.35.
- `scripts/parity_check.py`: C++ backtest vs research simulator, trade by trade.
- `scripts/state_report.py`: daily state summary; exit code 1 if any basket is halted.
- yfinance is flaky (missing tickers between runs). Check the downloaded set before
  comparing runs.

## Config

- `config/basket_configs/<id>.json`: tickers, weights, `trading_params` (window_size,
  entry_z, exit_z), research metrics. Don't hand-edit weights; regenerate from research.
- `config/risk.json`: fractions of USD equity at startup. `allocation_per_basket` 0.061 ≈
  $5,000, the capital the research sizes with; integer-share sizing depends on it.
- Control files: `state/KILL` (no new entries), `state/FLATTEN` (exit all at next session).

## Conventions

- Tests are offline and use the fakes in `tests/fakes/`: `SyncEventBus`,
  `FakeExecutionEngine`, `StrategyHarness` (`closeDay`, `openSession`, `fillFrom`, ...).
  Bug fixes get a regression test that fails before the fix.
- Comments explain why, briefly. Match the surrounding style.
- Never commit `state/`, `logs/`, `build/`, or credentials.
