// Shapes of the API responses (dashboard/backend/app/views.py). Times: `t`, `*_at` from
// the bot are epoch ms; `checked_at` / `state_saved_at` from the dashboard are epoch seconds.

export type MasterState = 'FLAT' | 'ENTRY_PENDING' | 'IN_POSITION' | 'EXIT_PENDING' | 'RECOVERY_PENDING' | 'SYSTEM_HALTED'
export type Side = 'LONG' | 'SHORT' | 'FLAT'

export interface BasketRow {
  id: string
  tickers: string[]
  entry_z: number
  exit_z: number
  window: number
  state: MasterState
  position: Side
  pending_action: string
  halt_reason: string | null
  decision_day: string | null
  close_z: number | null
  close_day: string | null
  intraday_z: number | null
  entry: Entry | null
  held_days: number | null     // trading sessions closed since the entry's
  realized: number
  unrealized: number | null
  commissions: number | null   // USD, from the broker's commission reports (null: none on record)
  in_books: boolean
  working_legs: number
  skipped_today: string | null
}

// The entry that opened the current position. restored: rebuilt by the bot for a position
// saved before it recorded entries (z recomputed from the warm-up history).
export interface Entry { day: string; decision_day: string | null; z: number | null; restored: boolean }

// A fill, compared with that day's official close (what the research assumes). cost > 0 = worse.
export interface Fill {
  t: number; basket: string; symbol: string; side: string; quantity: number; price: number; exec_id: string | null
  realized_pnl: number; close: number | null; cost_usd: number | null; cost_bps: number | null; commission: number | null
}
export interface FillQuality {
  fills: number; pending: number; cost_usd: number | null; bps: number | null
  worst: { basket: string; symbol: string; cost_bps: number; t: number } | null
}
export type PriceSource = 'close' | 'delayed' | null
export interface PriceSummary { source: PriceSource | 'mixed'; oldest_t: number | null; close_day: string | null; missing: number }
export interface TimelineEvent { t: number; type: string; basket: string | null; text: string }
// settling: books and broker differ, but not yet in two broker snapshots taken after the books last changed
export interface PositionRow { symbol: string; bot: number; broker: number | null; baskets: Record<string, number>; match: boolean | null; settling: boolean }
export interface Health { state: 'running' | 'stale' | 'silent' | 'stopped' | 'unknown'; running: boolean | null; age_s: number | null }
export interface Money { value: number; currency: string }

/** The Gateway's own link to IBKR. While it's cut off (error 1100) every socket stays up but
 * nothing trades. null: nobody can tell (bot stopped and no dashboard connection). */
export interface IbkrLink { connected: boolean | null; down_since: number | null }

export interface Overview {
  bot: Health
  broker: { connected: boolean; checked_at: number | null; bot_connected: boolean | null }
  live: { connected: boolean }
  ibkr: IbkrLink
  equity_usd: number | null
  net_liquidation?: Money
  realized: number
  unrealized: number | null
  prices: PriceSummary
  commissions: number | null
  commission_fills: number
  in_position: number
  gross_exposure_usd: number | null
  reserved_gross_usd: number | null
  gross_cap_usd: number | null
  flags: { kill: boolean; flatten: boolean }
  halted: { id: string; reason: string | null }[]
  mismatches: PositionRow[]
  settling: PositionRow[]
  working_orders: number
  baskets: BasketRow[]
  timeline: TimelineEvent[]
  fills: Fill[]
  fill_quality: FillQuality
  first_run: number | null
}

export interface Leg {
  symbol: string; weight: number; quantity: number; avg_price: number
  price: number | null; source: PriceSource; price_day: string | null; price_t: number | null
  realized: number; unrealized: number | null; broker_match: boolean | null; broker_settling: boolean
}
export interface Cycle {
  label: string; position: Side; day: string; t: number
  legs: { symbol: string; side: string; quantity: number; client_order_id: string }[]
  end_t: number | null; end_state: MasterState | null; filled: boolean | null
  fills: Fill[]
}
// action/position/state are null for days from the startup z history (before the journal began)
export interface Close { day: string; z: number | null; action: string | null; position: Side | null; state: MasterState | null }

export interface BasketDetail extends BasketRow {
  weights: Record<string, number>
  metrics: Record<string, number>
  exported_at: string | null
  periods: Record<string, { start: string; end: string }>
  legs: Leg[]
  closes: Close[]
  cycles: Cycle[]
  skipped: { t: number; reason: string }[]
  halts: { t: number; reason: string }[]
  fill_quality: FillQuality
  commission_fills: number
  loss_limit: number | null
  allocation_fraction: number | null
}

export interface BrokerOrder { symbol: string; side: string; quantity: number; type: string; status: string; order_ref: string; perm_id?: number; filled?: number }
export interface OrderRow {
  basket: string; symbol: string; side: string; quantity: number | null; client_order_id: string
  basket_state: MasterState | null; broker: BrokerOrder | null
  match: 'match' | 'missing_at_broker' | 'differs' | 'unknown_to_bot' | 'unknown'
}
// origin: 'bot' when orderRef carries a bot client order id, else placed by hand (TWS, mobile)
export interface Execution {
  exec_id: string; symbol: string; side: string; quantity: number; price: number; time: string; order_ref: string
  booked: boolean; origin: 'bot' | 'manual'; commission: number | null
}
export interface BrokerState { connected: boolean; checked_at: number | null; error: string | null }

export interface Orders {
  broker: BrokerState
  orders: OrderRow[]
  other_orders: BrokerOrder[]
  positions: PositionRow[]
  position_mismatches: PositionRow[]
  position_settling: PositionRow[]
  executions: Execution[]
}

// One distinct message in a bot run: logged `count` times, first at `time`, last at `last` (New York).
export interface LogLine { time: string; last: string; level: 'warning' | 'error'; message: string; count: number }
// A bot run (from the journal's start events; started_at null = before the first one).
export interface LogRun { started_at: number | null; current: boolean; lines: LogLine[] }
export interface SymbolCap { symbol: string; usd: number; pct: number | null; baskets: number }
export interface Halt { id: string; reason: string | null; halted_at: number | null; book_keys: Record<string, number> }

export interface System {
  bot: Health & { started_at: number | null; git_sha: string | null; broker_connected: boolean | null }
  can_edit_state: boolean
  broker: BrokerState
  live: { connected: boolean }
  ibkr: IbkrLink
  flags: { kill: boolean; flatten: boolean }
  halts: Halt[]
  last_close_t: number | null
  symbols: number
  market_data_lines: number
  state_saved_at: number | null
  equity_at_start_usd: number | null
  limits: { max_gross_exposure_usd: number; max_symbol_net_usd: number; max_basket_loss_usd: number } | null
  symbol_caps: SymbolCap[] | null   // null: the bot doesn't report it (older version)
  risk_config: Record<string, number | string>
  baskets_loaded: number | null
  baskets_configured: number
  logs: LogRun[]
}

// /api/session (backend app/nyse.py): today's session if it hasn't closed yet, else the next one.
export interface SessionInfo {
  open: boolean; today_is_session: boolean; holiday: string | null; early_close: boolean; day: string
  open_ms: number; close_ms: number; cutoff_ms: number; eod_ms: number; known: boolean
}

export interface Live {
  connected: boolean
  baskets: Record<string, { z: number | null }>
  prices: Record<string, number>
}
