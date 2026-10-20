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
  held_days: number | null
  realized: number
  unrealized: number | null
  in_books: boolean
  working_legs: number
  skipped_today: string | null
}

export interface Fill { t: number; basket: string; symbol: string; side: string; quantity: number; price: number; realized_pnl: number }
export interface TimelineEvent { t: number; type: string; basket: string | null; text: string }
export interface PositionRow { symbol: string; bot: number; broker: number | null; baskets: Record<string, number>; match: boolean | null }
export interface Health { state: 'running' | 'stale' | 'silent' | 'stopped' | 'unknown'; running: boolean | null; age_s: number | null }
export interface Money { value: number; currency: string }

export interface Overview {
  bot: Health
  broker: { connected: boolean; checked_at: number | null; bot_connected: boolean | null }
  live: { connected: boolean }
  equity_usd: number | null
  net_liquidation?: Money
  realized: number
  unrealized: number | null
  in_position: number
  gross_exposure_usd: number | null
  reserved_gross_usd: number | null
  gross_cap_usd: number | null
  flags: { kill: boolean; flatten: boolean }
  halted: { id: string; reason: string | null }[]
  mismatches: PositionRow[]
  working_orders: number
  baskets: BasketRow[]
  timeline: TimelineEvent[]
  fills: Fill[]
  first_run: number | null
}

export interface Leg {
  symbol: string; weight: number; quantity: number; avg_price: number; price: number | null
  realized: number; unrealized: number | null; broker_match: boolean | null
}
export interface Cycle {
  label: string; position: Side; day: string; t: number
  legs: { symbol: string; side: string; quantity: number; client_order_id: string }[]
  end_t: number | null; end_state: MasterState | null; filled: boolean | null
  fills: { t: number; symbol: string; side: string; quantity: number; price: number; exec_id: string; realized_pnl: number }[]
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
  loss_limit: number | null
  allocation_fraction: number | null
}

export interface BrokerOrder { symbol: string; side: string; quantity: number; type: string; status: string; order_ref: string; perm_id?: number; filled?: number }
export interface OrderRow {
  basket: string; symbol: string; side: string; quantity: number | null; client_order_id: string
  basket_state: MasterState | null; broker: BrokerOrder | null
  match: 'match' | 'missing_at_broker' | 'differs' | 'unknown_to_bot' | 'unknown'
}
export interface Execution { exec_id: string; symbol: string; side: string; quantity: number; price: number; time: string; order_ref: string; booked: boolean }
export interface BrokerState { connected: boolean; checked_at: number | null; error: string | null }

export interface Orders {
  broker: BrokerState
  orders: OrderRow[]
  other_orders: BrokerOrder[]
  positions: PositionRow[]
  position_mismatches: PositionRow[]
  executions: Execution[]
}

export interface LogLine { time: string; level: 'warning' | 'error'; message: string }
export interface Halt { id: string; reason: string | null; halted_at: number | null; book_keys: Record<string, number> }

export interface System {
  bot: Health & { started_at: number | null; git_sha: string | null; broker_connected: boolean | null }
  can_edit_state: boolean
  broker: BrokerState
  live: { connected: boolean }
  flags: { kill: boolean; flatten: boolean }
  halts: Halt[]
  last_close_t: number | null
  symbols: number
  market_data_lines: number
  state_saved_at: number | null
  equity_at_start_usd: number | null
  limits: { max_gross_exposure_usd: number; max_symbol_net_usd: number; max_basket_loss_usd: number } | null
  risk_config: Record<string, number | string>
  baskets_loaded: number | null
  baskets_configured: number
  logs: LogLine[]
}

export interface Live {
  connected: boolean
  baskets: Record<string, { z: number | null }>
  prices: Record<string, number>
}
