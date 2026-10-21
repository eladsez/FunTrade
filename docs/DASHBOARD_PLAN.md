# Dashboard: what's left (plan)

Status 2026-10-21: v1 is live on the Pi (`https://rpi.tail4f4240.ts.net:8443`, PR #4). PR #5
added the startup z history and removed the false startup MISMATCH report. **Phase 1 (1.1–1.9),
2.1 and 4.1 are done** on branch `dashboard-phase1` (marked ✅ below). This file lists
what is still missing for the dashboard to be **accurate**, **efficient** and **responsive**,
in the order it should be done. Each item says what is wrong today, how we know, the fix,
and the files involved. Tick items off here as they land.

Effort: **S** < 2 h · **M** half a day · **L** a day or more. **Bot** = needs a bot change
(full deploy after the close, regression test, parity check if it touches signal/timing).

---

## Phase 1: accuracy (numbers you can trust)

The dashboard is only useful if every number on it is right. These are the places where it
is wrong or silent today.

### 1.1 ✅ Entry z, entry day and "held" for positions opened before the journal — S, Bot
- **Today:** the 5 baskets entered on 2026-10-20 show "Entered at —" and no held days: their
  entry cycle happened before the journal existed, and nothing else records it.
- **Fix:** add `entry_day` and `entry_z` to the basket state file (set in `enter()`, cleared
  on FLAT). For the 5 existing positions, backfill once from today's `ENTER_*` close in
  `state/zhistory` (decision day 2026-10-19). Dashboard prefers the state file, then the
  journal's ENTRY cycle.
- **Files:** `BasketMeanReversionStrategy.{hpp,cpp}` (saveState/loadState), `views.py row()`,
  `Basket.tsx`.

### 1.2 ✅ "Held" counts calendar days — S
- **Today:** `held_days = today − entry day` in calendar days; a Friday entry shows 3 on Monday.
- **Fix:** count trading days from the journal/zhistory day list (the bot's own calendar).
  Show "3 trading days".
- **Files:** `views.py row()`.

### 1.3 ✅ Commissions are missing from P&L — M
- **Today:** P&L is gross. Paper charges ~$1 per leg (seen in `commissionReport`), so a round
  trip costs ~$6 per 3-leg basket. With ~$100 typical trade P&L, that is material.
- **Fix:** the broker view subscribes to `commissionReport` (already arrives with
  `reqExecutions`), stores commission per `exec_id` in `dashboard/commissions.jsonl` (the
  broker only returns recent days), and the dashboard shows net P&L next to gross.
- **Files:** `broker.py` (callback + persistence), `views.py`, `Overview.tsx`, `Basket.tsx`.

### 1.4 ✅ Fill quality vs the research assumption — M
- **Today:** paper fills MOC at ~15:59:30 at the last price, not the official close (CLAUDE.md).
  Nothing shows how far fills are from the close the research assumes.
- **Fix:** per fill, show the official close of that day and the difference in $ and bps.
  Closes come from the bot: add each day's leg closes to the journal `close` event
  (bot change, display only). Summarize per basket and overall on the Basket and a new
  "Paper vs research" card.
- **Files:** `BasketMeanReversionStrategy.cpp decide()` (journal fields), `views.py`, `Basket.tsx`.

### 1.5 ✅ Transient false mismatches around fills — S
- **Today:** the broker snapshot is up to 60 s old. Right after the 16:00 fills, books and
  broker can disagree for one poll and the Overview shows a red mismatch banner.
- **Fix:** report a position mismatch only when it is present in two consecutive broker
  snapshots taken after the latest journaled fill; otherwise show "settling".
- **Files:** `views.py orders()`, `broker.py` (keep the previous snapshot).

### 1.6 ✅ Market calendar: holidays and half days — S
- **Today:** "US session open" is weekday 09:30–16:00 ET. Wrong on NYSE holidays and on half
  days (13:00 close, MOC cutoff 12:50).
- **Fix:** backend serves `/api/session` from a hard-coded NYSE calendar for 2026–2027
  (holidays + early closes; ~20 dates, updated yearly), frontend uses it instead of
  computing locally. Same list could later replace guesses in the bot.
- **Files:** new `app/calendar.py`, `time.ts session()`.

### 1.7 ✅ Live prices are delayed quotes — S
- **Today:** open P&L uses the bot's live feed, which is IBKR *delayed* data (no
  subscription). It can be ~15 min old in session and is the last delayed print after hours.
- **Fix:** label it ("delayed ~15 min", time of the last price per leg); after 16:00 use the
  day's close from the journal (1.4) instead of the feed.
- **Files:** `live.py` (timestamp per price), `views.py legs_of()`, UI labels.

### 1.8 ✅ Per-symbol risk usage — S, Bot (small)
- **Today:** 7 HBAN/SCHW baskets were rejected by the per-symbol cap; the dashboard shows the
  rejection text but not how full each symbol's cap is.
- **Fix:** add `symbol_net_usd` (per symbol, incl. reservations) to `status.json`; System page
  shows a sorted bar list (HBAN 96 % of cap, …). This is also the input for the open
  "smarter basket selection" item in CLAUDE.md.
- **Files:** `main.cpp writeStatus`, `RiskManager.hpp` (accessor), `System.tsx`.

### 1.9 ✅ Warnings grouped by bot run — S
- **Today:** the System page listed the last 100 warning/error lines regardless of run, so a
  problem a restart fixed (the 16:55 false MISMATCH) looked current, and warnings repeated at
  every start (manual RapidOrder executions) filled the list.
- **Fix:** lines are assigned to runs by the journal's `start` events and folded (×count,
  first/last time); the current run is open, earlier runs collapsed.
- **Files:** `logs.py`, `System.tsx`.

### Phase 1 as built (notes)
- 1.1: bot `EntryRecord` (decision day, entry day, z) persisted while not FLAT; positions saved
  before it existed are restored at startup from `cycle_day` and the close before it (flagged
  `restored`). The 5 positions held on 2026-10-21 get it on the next deploy.
- 1.3: commissions attributed by orderRef (`<basket>_<SYM>#…`) or the journal's fill; non-USD
  kept apart. The 2026-10-20 entry fills predate the dashboard's commission log, so their
  fees show only if IBKR still returns those executions.
- 1.4: needs the bot's new journal field (`closes` on `close` events); fills from before it
  show "close pending".
- 1.7: the bot's startup seed (last close) is treated as a close, not a quote.
- 1.8: `RiskManager::symbolNetUsd()` → `status.json symbol_net_usd`.
- 2.1: `components/ResponsiveTable.tsx` (table ≥ md, cards below) used by Overview fills,
  Basket legs/trades, Orders (all three tabs). Manual executions are shown neutrally.
- 4.1: Vitest + Testing Library, 71 tests (lib, components, every page, the live stream),
  ~99% lines. Backend: 88 tests, new modules at 100%.

---

## Phase 2: phone (the main way it's used)

### 2.1 ✅ Card layouts instead of scrolling tables — M
- **Today:** Orders, Legs, Trades and Executions are tables; on a 390 px screen the Match /
  P&L columns are off-screen (screenshots 2026-10-20).
- **Fix:** below `md`, render each row as a card with the status first (✓ / mismatch / not
  booked), then symbol, side, qty. One shared `ResponsiveTable` component (columns declare
  which fields show on the card) so desktop and phone stay in sync.
- **Files:** new `components/ResponsiveTable.tsx`; `Orders.tsx`, `Basket.tsx`, `Overview.tsx`.

### 2.2 Installable app and offline shell — M
- **Today:** manifest with an SVG icon only. iOS needs PNG `apple-touch-icon`; there is no
  service worker, so a closed tunnel shows a browser error page.
- **Fix:** PNG icons (180/192/512, maskable), `vite-plugin-pwa` like Kupa (app shell cached,
  API network-first, `/api/events` never cached), an "offline / last updated hh:mm" banner.
- **Files:** `vite.config.ts`, `public/icons/`, `App.tsx`.

### 2.3 Push notifications for things that need a human — L
- **Today:** you only learn about a halt, a mismatch or a silent bot by opening the page.
- **Fix:** Web Push (works on the tailnet HTTPS origin, iOS 16.4+ once installed). Backend
  watches for: new halt, mismatch persisting (1.5), bot heartbeat > 5 min in session, broker
  disconnected > 10 min, MOC orders not at the broker by 15:45 ET, fills not booked. One
  notification per event, with a quiet period. VAPID keys in `dashboard/` on the Pi (not git).
- **Files:** new `app/alerts.py`, `app/push.py` (pywebpush), service worker, Settings toggle.

### 2.4 Small phone fixes — S
- Tap targets ≥ 44 px on filter pills (now 32 px).
- Sticky filter bar on the Baskets page.
- Pull-to-refresh equivalent: tap the status dot to refetch.
- Self-host Inter / JetBrains Mono (now Google Fonts: an extra origin, slow first load on mobile data).

---

## Phase 3: efficiency (Pi 4, phone on mobile data)

Measured on sample data: API responses are ~30 KB (overview) and the bundle is 357 KB
(110 KB gzipped). Fine today; these keep it fine as history grows.

### 3.1 Cache the request context — S
- **Today:** every request re-reads all state files, and `/api/overview` calls `orders()`
  inside (positions.json read twice). Each open browser refetches the overview every 10 s
  while live prices move.
- **Fix:** build `Ctx` once per (state_version, broker.version) and reuse it; recompute only
  live-price-dependent fields (P&L) per request. Add ETag/304 so unchanged pages cost nothing.
- **Files:** `main.py`, `views.py`.

### 3.2 Push P&L instead of refetching — M
- **Today:** a `live` event triggers a full overview refetch (throttled to 10 s).
- **Fix:** send per-basket P&L and z in the `live` event (small diff); the frontend patches
  the cached overview. Refetch only on `state` / `broker` events.
- **Files:** `main.py events()`, `live.tsx`.

### 3.3 Journal growth — S
- ~10 KB/day now (61 baskets); kept fully in memory. Fine for years, but the startup parse
  grows. **Fix:** monthly files `state/journal/YYYY-MM.jsonl` (bot), index loads the last
  13 months, older on demand. Do it when the file passes ~20 MB.

### 3.4 Smaller bundle — S
- Split pages with `React.lazy` (Basket/Orders/System load on demand), keep the Overview in
  the first chunk. Target < 70 KB gzipped for the first load.

---

## Phase 4: correctness of the dashboard itself

### 4.1 ✅ Frontend tests — M
- **Today:** none. The logic most likely to be wrong is in plain TS: `session()`, `note()`,
  exit-z wording (negative exit_z caused "−−0.25" before), formatting.
- **Fix:** Vitest unit tests for `lib/time.ts`, `lib/format.ts`, `BasketList note()`; run in
  the image build like the backend tests.

### 4.2 Visual smoke test in CI — M
- Playwright against the sample data at 1440 px and 390 px: every page renders, no console
  errors, no horizontal scroll on phone (the 451 px overflow bug). Runs in the dashboard
  image build or a CI job; screenshots kept as artifacts.

### 4.3 Write access — S
- **Today:** anyone on the tailnet can press KILL / FLATTEN (audited with the tailnet login).
- **Fix:** allowlist of tailnet logins for writes (`DASHBOARD_WRITERS`), others read-only.
  Optional passkey step for FLATTEN and Clear halt (Kupa has the code).

### 4.4 Dashboard-only deploy path, first real run — S
- `DASHBOARD_ONLY=1` in `deploy.sh` has not run yet (PR #4 was a full deploy). The first
  dashboard-only PR should be watched; check that the bot's container start time does not
  change.

---

## Phase 5: features from the original plan

### 5.1 Paper vs research — L
The exit criterion for real money is 4–6 clean paper weeks. Show it:
- per basket: live trades next to what `compute.simulate_event_loop` produces on the same
  dates (run nightly in the dashboard container on IBKR history, no yfinance), differences
  in entry/exit days and P&L;
- weekly scorecard: mismatches, halts, unexplained events, P&L vs research.

### 5.2 Basket set comparison — M
- For the planned re-selection (CLAUDE.md open items): load a second config folder, show
  overlap and holdout metrics side by side before switching.

### 5.3 Intraday z on the chart — S
- The last point of the chart as a dashed marker at the current intraday z (display only).

---

## Order of work

1. 1.1, 1.2, 1.5, 1.6, 1.7 (accuracy fixes, small) and 2.1 (phone tables) — one dashboard PR
   plus one small bot PR (1.1), deployed after a close.
2. 1.3, 1.4, 1.8 (commissions, fill quality, symbol caps).
3. 2.2, 2.4, 3.1, 3.2, 4.1, 4.2 (installable app, efficiency, tests).
4. 2.3 (push notifications), 4.3.
5. 5.1–5.3.

Every bot change keeps the rules in CLAUDE.md: regression test first, journal/status stay
write-only for the bot, no signal/timing/sizing change without the parity check.
