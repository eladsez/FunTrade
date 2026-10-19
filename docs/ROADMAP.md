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

Caveats: one 7-month period; simulator is daily-close (not the C++ signal); baskets
share symbols; only 3/61 spreads still pass ADF on 140 days (low test power, but watch it).
Gate passed: keep the baskets. Re-run `holdout_check.py` monthly to watch for decay.

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

- [ ] Persist per-basket state (master state, side, leg quantities) to disk on every transition
- [ ] On startup: load state, reconcile against broker positions, refuse to trade on mismatch
- [ ] Shared-symbol netting: per-basket virtual positions must sum to the broker position

**Gate:** kill -9 mid-cycle, restart, and the bot resumes or halts safely with no orphaned legs.

## Phase 4 — Signal parity

- [ ] Decide the signal (daily close vs intraday) and make research and C++ compute it identically
- [ ] Warm-up: refuse to trade until the window is genuinely full; no zero-filled history
- [ ] Advance the window exactly once per trading day
- [ ] Same price basis in research and live (raw vs dividend-adjusted)
- [ ] Deterministic replay backtest through the real C++ strategy; cross-check against Python on the same data

**Gate:** C++ backtest and Python simulation produce the same trades on the same data.

## Phase 5 — Robustness and risk

- [ ] IBKR reconnect without use-after-free; handle 1100/1101/1102; re-subscribe; re-sync
- [ ] Thread-safety: all strategy state mutated on the dispatcher thread only (incl. shutdown)
- [ ] Global risk layer: gross exposure cap, per-basket loss limit, per-symbol concentration cap
- [ ] Market-hours guard, kill switch, halt-on-anomaly; alerts
- [ ] Order watchdog: legs unresolved after N seconds are cancelled/queried (covers unhandled IBKR error codes)

**Gate:** chaos tests (drop connection mid-cycle, rejects, stale data) end in a correct or safely halted state.

## Phase 6 — Paper trading

- [ ] Run on IBKR paper for at least 4–6 weeks; daily reconciliation report
- [ ] Track live vs simulated PnL per basket

**Gate:** no position mismatches, live tracks simulation. Only then consider real money,
starting small. That decision stays with the owner.
