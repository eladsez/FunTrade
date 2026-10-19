# FunTrade Hardening Roadmap

Goal: a bot that runs unattended on IBKR paper, keeps positions correct through
disconnects and restarts, and trades the same signal that research validated.
Alpaca code stays in the repo but is no longer maintained or tested.

Each phase ends with a gate. Don't start the next phase until the gate passes.

## Phase 0 — Does the edge exist? (Python)

Re-run the 62 exported baskets, unchanged, on data after 2026-03-28 (export date).
None of that data was used to select them, so it is a genuine holdout.

- [x] `scripts/holdout_check.py`: same `simulate_event_loop`, same weights/thresholds, no re-tuning
- [x] Report per basket: Sharpe, PnL, trades, still cointegrated?; plus the aggregate distribution
- [x] Control: random same-sector baskets through the same pipeline without selection filters

**Result (2026-10-19, holdout 2026-03-30 → 2026-10-19, 140 trading days): edge looks real.**

| | Positive | Median Sharpe | 60-basket portfolio Sharpe |
|---|---|---|---|
| 61 selected baskets (EQR missing data) | 43/60 traded (72%) | 0.73 (research OOS: 1.16) | 2.92, t-stat 2.17 |
| 496 random baskets, no selection filters | 55% | 0.11 | median 0.35, 95th pct 1.62, never ≥ 2.92 in 2000 draws |

Caveats: one 7-month period; baskets share symbols; only 3/61 spreads still pass ADF on
140 days (low test power, but watch it). Gate passed: keep the baskets.

**Update (Phase 4):** the research simulator sized entries with the fill day's close, which
no live system can know. With sizing on the previous close (what the bot does; now the
default in `compute.py`), on one complete download of all 91 tickers:

| Holdout 2026-03-30 → 2026-10-19 | Positive | Median Sharpe | Portfolio Sharpe | t-stat |
|---|---|---|---|---|
| Original (fill-day sizing) | 43/61 | 0.60 | 2.73 | 2.04 |
| Achievable (previous-close sizing) | 40/60 | 0.58 | 2.50 | 1.86 |

The edge survives realistic execution but is borderline significant (p ≈ 0.06). The 2.92
above came from a download missing EQR. Re-run `holdout_check.py` monthly to watch for decay.

**Gate:** decides the strategy track.
- Clear edge (most baskets positive after costs) → keep the baskets, continue.
- No edge (results centred near zero) → harden the engine anyway, but plan a
  re-research phase (proper holdout, multiple-testing control, point-in-time universe).

## Phase 1 — Build and test foundations

- [x] Fix the build: document Boost/Protobuf as system deps; removed unused Eigen (it registered 926 of its own tests)
- [x] Real unit tests for `BasketState`, `BasketSizer`, `ExecutionCycle`, `PortfolioManager`, `OrderManager`, `EventBus`
- [x] End-to-end strategy test (entry, fill, exit, whiff, partial-fill recovery) on `tests/fakes/`
- [x] Known bugs captured as `DISABLED_PhaseN_*` tests (11); each fails today when forced to run
- [x] Live broker tests opt-in (`-DFUNTRADE_LIVE_TESTS=ON`); `compute.py` fixed for complex Johansen eigenvectors
- [ ] Moved to Phase 2: scripted fake for IBKR callbacks (needs a seam between `IbkrExecutionEngine` and `IbkrInfra`)

**Gate:** `ctest` runs in seconds with no network and covers the core logic. ✅ 43 tests, 0.2 s.

## Phase 2 — Order lifecycle correctness (IBKR)

- [x] One ID scheme: `<position_key>#<suffix>`, unique per order; engines send and report it unchanged
- [x] IBKR: single source of fills (`execDetails`), dedupe by `execId`; `orderStatus` for status only
      (`IbkrOrderTracker`: pure logic, unit tested with IBKR-shaped callback sequences)
- [x] Fully initialise every `ExecutionReport` (default member initialisers) and add `exec_id`
- [x] Never overwrite filled qty with 0; terminal states are sticky in leg, OrderManager and tracker
- [x] Terminal states only from real terminal events; `PendingCancel` is no longer terminal
- [x] Terminal status held back until its executions arrive (positions complete before strategy reacts)
- [x] Subscribers dispatched in subscription order (PortfolioManager books fills before strategies)
- [x] Bounded order-ID wait (5 s); failed placements and Alpaca HTTP rejections resolve as REJECTED
- [x] Empty exit cycle completes immediately (→ FLAT); recovery state set before dispatch
- [x] `stop()` while pending: cancels working orders, unwinds partial fills, returns promptly
- [x] OrderManager: registers before sending (no clobber), never calls the engine under its lock
- [x] Mock engine IDs fixed; account values converted from base currency (ILS) to USD
- [ ] Verify tracker assumptions against a real IB paper gateway (`-DFUNTRADE_LIVE_TESTS=ON`) — needs the owner's gateway

**Gate:** fake-engine tests for every fill/cancel/reject sequence pass; positions always match.
✅ 77 tests (0.23 s), clean under ASan/UBSan. Remaining risk: IBKR callback orderings seen
live that the tests don't model, and error codes outside the handled set (Phase 5 watchdog).

## Phase 3 — State persistence and reconciliation

- [x] Atomic JSON state (`utils/StateStore.hpp`: temp file, fsync, rename); corrupt state refuses to start
- [x] PortfolioManager persists per-basket books and booked execution ids after every fill
- [x] Broker positions kept separate from virtual books; per-symbol reconciliation at startup and daily
- [x] Strategies persist state, pending action, decision day and in-flight legs on every transition
- [x] Startup: replay today's executions (`reqExecutions`), reconcile, halt on mismatch, reattach
      in-flight cycles (fills derived from book movement; working orders keep reporting)
- [x] Halts persist across restarts and need a human (see `docs/OPERATIONS.md`)
- [x] Graceful stop no longer flattens: cancels working orders, saves state, positions resume on restart

**Gate:** kill mid-cycle, restart, resume or halt safely. ✅ `tests/test_restart.cpp` (8 scenarios:
graceful restart in position, pending decision runs once, crash with a working order, crash after a
partial, broker mismatch, persistent halt, redelivered execution, corrupt state).

## Phase 4 — Signal parity

- [x] Signal = research rule: z of the daily close over W complete days incl. that day (sample std);
      decide at close d, act at session d+1 with MARKET_ON_CLOSE orders (fill at close d+1)
- [x] `BasketState` keyed by trading day: re-sent closes overwrite, no z until W real days exist
- [x] Intraday EMA signal removed (indicative intraday z kept for the UI only)
- [x] Live warm-up and daily window rebuild from dividend-ADJUSTED IBKR history (research uses adjusted)
- [x] Deterministic backtest: synchronous event bus; mock exchange fills MOC at the session's close
- [x] Fixed: a close arriving while a cycle was still filling consumed that day's decision (found by parity)
- [x] Research simulator sizes entries with the previous close (the only price known when ordering)

**Gate:** C++ backtest and Python simulation produce the same trades on the same data.
✅ `scripts/parity_check.py --size-with-prev-close`: **60/60 baskets identical** (days, legs, sides,
fill prices) on the 2026 holdout.

## Phase 5 — Robustness and risk

- [x] All strategy state on the dispatcher thread (`IEventBus::post`/`flush`), including start-up
      reconciliation and shutdown
- [x] TWS client access serialised (`ClientGuard`): the TWS send path is not thread-safe
- [x] IBKR reconnect reuses one client object (no use-after-free); 1100 logged, 1101 re-subscribes
- [x] `RiskManager`: gross exposure cap, per-symbol net cap, reservations for in-flight MOC entries,
      per-basket loss limit (exit then halt), kill-switch and flatten files (`config/risk.json`)
- [x] Order watchdog: cycles unresolved a full session later are cancelled at the next close
- [x] Rotating file log with errors flushed immediately; orchestrator refuses to size without equity
- [x] ASan/UBSan and TSan clean on the full unit suite
- [ ] Not done: alerts beyond logs/exit codes (e-mail/webhook), stale-price/bad-tick detection,
      explicit MOC cutoff guard (late orders are rejected by the exchange and simply re-decided)

**Gate:** failure scenarios end correct or safely halted. ✅ covered by the strategy and restart
suites (rejects, partials, failed recovery, stale cycles, kill/flatten, loss limit). Reconnect and
1101 handling are implemented but only verifiable against a real gateway.

## Phase 6 — Paper trading (owner)

- [x] Runbook: `docs/OPERATIONS.md`; daily report: `scripts/state_report.py` (exit 1 on halts)
- [ ] Build with `-DFUNTRADE_LIVE_TESTS=ON`, run `tests/test_ibkr_execution` against IB Gateway paper
- [ ] Run on IBKR paper for at least 4–6 weeks; daily reconciliation report must show 0 mismatches
- [ ] Track live vs simulated PnL per basket; one deliberate restart and one disconnect handled

**Gate:** no position mismatches, live tracks simulation. Only then consider real money,
starting small. That decision stays with the owner.
