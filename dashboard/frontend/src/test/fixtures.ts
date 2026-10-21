// API responses for component and page tests, shaped like dashboard/backend/app/views.py.
import type { BasketDetail, BasketRow, Fill, FillQuality, Orders, Overview, SessionInfo, System } from '../lib/types'

const ny = (iso: string) => Date.parse(`${iso}-04:00`)
export const NOW = ny('2026-10-21T14:05')

export const session: SessionInfo = {
  open: true, today_is_session: true, holiday: null, early_close: false, day: '2026-10-21',
  open_ms: ny('2026-10-21T09:30'), close_ms: ny('2026-10-21T16:00'), cutoff_ms: ny('2026-10-21T15:50'),
  eod_ms: ny('2026-10-21T18:00'), known: true,
}

export function row(over: Partial<BasketRow> = {}): BasketRow {
  return {
    id: 'ajg_hban_schw', tickers: ['AJG', 'HBAN', 'SCHW'], entry_z: 2, exit_z: 0.25, window: 42,
    state: 'IN_POSITION', position: 'LONG', pending_action: 'NONE', halt_reason: null, decision_day: '2026-10-20',
    close_z: -1.85, close_day: '2026-10-20', intraday_z: null,
    entry: { day: '2026-10-20', decision_day: '2026-10-19', z: -2.366, restored: true },
    held_days: 1, realized: 0, unrealized: 41.5, commissions: 3.12, in_books: true, working_legs: 0, skipped_today: null,
    ...over,
  }
}

export function fill(over: Partial<Fill> = {}): Fill {
  return {
    t: ny('2026-10-20T15:59'), basket: 'ajg_hban_schw', symbol: 'HBAN', side: 'SELL', quantity: 133, price: 15.98,
    exec_id: 'e-hban', realized_pnl: 0, close: 16.01, cost_usd: 3.99, cost_bps: 18.7, commission: 1.04, ...over,
  }
}

export const quality: FillQuality = {
  fills: 15, pending: 0, cost_usd: 12.4, bps: 3.2, worst: { basket: 'ajg_hban_schw', symbol: 'HBAN', cost_bps: 18.7, t: 0 },
}

export function overview(over: Partial<Overview> = {}): Overview {
  return {
    bot: { state: 'running', running: true, age_s: 20 },
    broker: { connected: true, checked_at: NOW / 1000 - 30, bot_connected: true },
    live: { connected: true },
    equity_usd: 81994, net_liquidation: { value: 253648, currency: 'ILS' },
    realized: 0, unrealized: 41.5,
    prices: { source: 'delayed', oldest_t: ny('2026-10-21T13:48'), close_day: null, missing: 0 },
    commissions: 15.6, commission_fills: 15,
    in_position: 5, gross_exposure_usd: 20101, reserved_gross_usd: 0, gross_cap_usd: 82076,
    flags: { kill: false, flatten: false }, halted: [], mismatches: [],
    settling: [{ symbol: 'IP', bot: -53, broker: 0, baskets: { ball_ip_sw: -53 }, match: false, settling: true }],
    working_orders: 0, baskets: [row()], timeline: [], fills: [fill()], fill_quality: quality, first_run: ny('2026-10-20T13:30'),
    ...over,
  }
}

export function detail(over: Partial<BasketDetail> = {}): BasketDetail {
  return {
    ...row(),
    weights: { AJG: 0.02, HBAN: -1, SCHW: 0.17 }, metrics: { oos_sharpe: 2.1, train_hl: 9.3, train_p: 0.01, oos_p: 0.03 },
    exported_at: '2026-03-28T00:00:00', periods: {},
    legs: [
      { symbol: 'AJG', weight: 0.02, quantity: 2, avg_price: 259.44, price: 261.0, source: 'delayed', price_day: null,
        price_t: ny('2026-10-21T13:50'), realized: 0, unrealized: 3.12, broker_match: true, broker_settling: false },
      { symbol: 'HBAN', weight: -1, quantity: -133, avg_price: 15.98, price: 15.7, source: 'close', price_day: '2026-10-20',
        price_t: null, realized: 0, unrealized: 37.24, broker_match: false, broker_settling: true },
      { symbol: 'SCHW', weight: 0.17, quantity: 23, avg_price: 70.1, price: null, source: null, price_day: null,
        price_t: null, realized: 0, unrealized: null, broker_match: false, broker_settling: false },
    ],
    closes: [{ day: '2026-10-19', z: -2.366, action: null, position: null, state: null },
             { day: '2026-10-20', z: -1.85, action: 'NONE', position: 'LONG', state: 'IN_POSITION' }],
    cycles: [{ label: 'ENTRY', position: 'LONG', day: '2026-10-20', t: ny('2026-10-20T09:31'), legs: [],
               end_t: ny('2026-10-20T16:00'), end_state: 'IN_POSITION', filled: true, fills: [fill()] }],
    skipped: [], halts: [], fill_quality: { ...quality, fills: 3, pending: 1 }, commission_fills: 3,
    loss_limit: 751, allocation_fraction: 0.061,
    ...over,
  }
}

export function orders(over: Partial<Orders> = {}): Orders {
  return {
    broker: { connected: true, checked_at: NOW / 1000 - 30, error: null },
    orders: [], other_orders: [],
    positions: [
      { symbol: 'HBAN', bot: -133, broker: -133, baskets: { ajg_hban_schw: -133 }, match: true, settling: false },
      { symbol: 'IP', bot: -53, broker: 0, baskets: { ball_ip_sw: -53 }, match: false, settling: true },
      { symbol: 'TRV', bot: 2, broker: 0, baskets: { hban_schw_trv: 2 }, match: false, settling: false },
    ],
    position_mismatches: [], position_settling: [],
    executions: [
      { exec_id: 'e1', symbol: 'HBAN', side: 'SLD', quantity: 133, price: 15.98, time: '20261021  15:59:30',
        order_ref: 'ajg_hban_schw_HBAN#1-1', booked: true, origin: 'bot', commission: 1.04 },
      { exec_id: 'e2', symbol: 'GWW', side: 'BOT', quantity: 1, price: 1101, time: '20261021  10:13:48',
        order_ref: 'RapidOrder', booked: false, origin: 'manual', commission: null },
      { exec_id: 'e3', symbol: 'SW', side: 'BOT', quantity: 60, price: 38.1, time: '20261021  15:59:31',
        order_ref: 'ball_ip_sw_SW#1-3', booked: false, origin: 'bot', commission: null },
    ],
    ...over,
  }
}

export function system(over: Partial<System> = {}): System {
  return {
    bot: { state: 'running', running: true, age_s: 20, started_at: ny('2026-10-20T17:34'), git_sha: 'ca784ef4e96e', broker_connected: true },
    can_edit_state: false, broker: { connected: true, checked_at: NOW / 1000 - 30, error: null }, live: { connected: true },
    flags: { kill: false, flatten: false }, halts: [], last_close_t: ny('2026-10-20T18:05'),
    symbols: 92, market_data_lines: 100, state_saved_at: NOW / 1000 - 60, equity_at_start_usd: 82076,
    limits: { max_gross_exposure_usd: 82076, max_symbol_net_usd: 8207, max_basket_loss_usd: 751 },
    symbol_caps: Array.from({ length: 10 }, (_, i) => ({ symbol: `S${i}`, usd: 8000 - i * 700, pct: (8000 - i * 700) / 8207, baskets: 10 - i })),
    risk_config: { allocation_per_basket: 0.061, max_gross_exposure: 1, max_symbol_net: 0.1, max_basket_loss: 0.15 },
    baskets_loaded: 61, baskets_configured: 61,
    logs: [
      { started_at: ny('2026-10-20T17:34'), current: true, lines: [
        { time: '2026-10-20 17:34:18', last: '2026-10-20 18:00:00', level: 'warning', count: 2,
          message: "Execution 00025b44 for untracked order 0 (ref 'RapidOrder', BOT 1 GWW) ignored." }] },
      { started_at: ny('2026-10-20T16:55'), current: false, lines: [
        { time: '2026-10-20 16:55:08', last: '2026-10-20 16:55:08', level: 'error', count: 1,
          message: 'MISMATCH IP: bot books -53.0000, broker holds 0.0000.' }] },
    ],
    ...over,
  }
}
