import { useState } from 'react'
import { Link } from 'react-router-dom'
import { useOrders } from '../lib/api'
import { qty, usd } from '../lib/format'
import { ago } from '../lib/time'
import type { Execution, OrderRow, PositionRow } from '../lib/types'
import { Card, Dot, Failed, Loading, sideClass } from '../components/ui'
import { ResponsiveTable, type Col } from '../components/ResponsiveTable'

type Tab = 'orders' | 'positions' | 'exec'

const MATCH: Record<OrderRow['match'], [string, string]> = {
  match: ['✓', 'text-good'],
  missing_at_broker: ['not at broker', 'text-bad font-semibold'],
  differs: ['quantity/side differs', 'text-bad font-semibold'],
  unknown_to_bot: ['unknown to the bot', 'text-bad font-semibold'],
  unknown: ['broker not checked', 'text-muted'],
}

const ORDER_COLS: Col<OrderRow>[] = [
  { key: 'basket', head: 'Basket', cell: r => <Link to={`/baskets/${r.basket}`}>{r.basket}</Link>, className: 'num text-[13px]', card: 'title' },
  { key: 'leg', head: 'Leg', cell: r => r.symbol, className: 'num font-medium', card: 'title' },
  { key: 'side', head: 'Side', cell: r => r.side, className: r => sideClass(r.side), card: 'title' },
  { key: 'qty', head: 'Qty', right: true, className: 'num',
    cell: r => <>{r.quantity ?? qty(r.broker?.quantity, false)}{r.broker && r.quantity != null && r.broker.quantity !== r.quantity && <span className="text-bad"> (broker {r.broker.quantity})</span>}</> },
  { key: 'type', head: 'Type', cell: r => r.broker?.type ?? 'MOC', className: 'num text-dim', card: false },
  { key: 'state', head: 'Basket state', cell: r => r.basket_state ?? '—', className: 'num text-[13px]' },
  { key: 'status', head: 'Broker status', cell: r => r.broker?.status ?? '—', className: 'num text-[13px]' },
  { key: 'ref', head: 'orderRef', cell: r => r.client_order_id, className: 'num text-xs text-muted', card: false },
  { key: 'match', head: 'Match', cell: r => MATCH[r.match][0], className: r => `text-[13px] ${MATCH[r.match][1]}`, card: 'aside' },
]

const posStatus = (p: PositionRow): [string, string] =>
  p.match == null ? ['broker not checked', 'text-muted'] : p.match ? ['Match', 'text-good']
    : p.settling ? ['Rechecking', 'text-warn'] : ['Mismatch', 'font-semibold text-bad']

const POSITION_COLS: Col<PositionRow>[] = [
  { key: 'sym', head: 'Symbol', cell: p => p.symbol, className: 'num font-medium', card: 'title' },
  { key: 'bot', head: 'Bot books (all baskets)', label: 'Bot books', right: true, cell: p => qty(p.bot), className: 'num' },
  { key: 'broker', head: 'Broker', right: true, cell: p => qty(p.broker), className: 'num' },
  { key: 'baskets', head: 'Baskets', cell: p => Object.entries(p.baskets).map(([b, q]) => `${b} ${qty(q)}`).join(' · ') || '—',
    className: 'num text-[13px] whitespace-normal text-dim' },
  { key: 'status', head: 'Status', cell: p => posStatus(p)[0], className: p => `text-[13px] ${posStatus(p)[1]}`, card: 'aside' },
]

const side = (s: string) => (s === 'BOT' ? 'BUY' : s === 'SLD' ? 'SELL' : s)
// Executions the bot placed must be booked; trades placed by hand (TWS, mobile) are shown, not flagged.
const booked = (e: Execution): [string, string] =>
  e.origin === 'manual' ? ['manual trade', 'text-muted'] : e.booked ? ['✓', 'text-good'] : ['not booked', 'font-semibold text-bad']

const EXEC_COLS: Col<Execution>[] = [
  { key: 'time', head: 'Time (ET)', cell: e => e.time.trim().split(/\s+/).pop(), className: 'num text-[13px] text-muted' },
  { key: 'sym', head: 'Symbol', cell: e => e.symbol, className: 'num font-medium', card: 'title' },
  { key: 'side', head: 'Side', cell: e => side(e.side), className: e => sideClass(side(e.side)), card: 'title' },
  { key: 'qty', head: 'Qty @ price', right: true, cell: e => `${e.quantity} @ ${e.price.toFixed(2)}`, className: 'num' },
  { key: 'fee', head: 'Commission', right: true, cell: e => (e.commission == null ? '—' : usd(e.commission, { cents: true })), className: 'num text-dim' },
  { key: 'ref', head: 'orderRef', cell: e => e.order_ref || '—', className: 'num text-xs text-muted', card: false },
  { key: 'booked', head: 'Booked', cell: e => booked(e)[0], className: e => `text-[13px] ${booked(e)[1]}`, card: 'aside' },
]

export default function Orders() {
  const { data: o, error, isLoading } = useOrders()
  const [tab, setTab] = useState<Tab>('orders')
  if (isLoading) return <Loading />
  if (error || !o) return <Failed error={error} />

  const checked = o.broker.checked_at ? o.broker.checked_at * 1000 : null
  const bad = o.orders.filter(r => r.match !== 'match' && r.match !== 'unknown').length
  const notBooked = o.executions.filter(e => e.origin === 'bot' && !e.booked).length
  const manual = o.executions.filter(e => e.origin === 'manual').length
  const badRow = 'bg-[#24141A]'

  const tabs: { id: Tab; label: string; count: number }[] = [
    { id: 'orders', label: 'Working orders', count: o.orders.length },
    { id: 'positions', label: 'Positions', count: o.positions.length },
    { id: 'exec', label: 'Executions today', count: o.executions.length },
  ]

  return (
    <>
      <div className="flex flex-wrap items-baseline gap-x-4 gap-y-2">
        <h1 className="m-0 text-[22px] font-semibold">Bot vs broker</h1>
        <span className="text-sm text-muted">
          Broker view from a read-only IBKR connection (its own client id) ·{' '}
          {o.broker.connected ? `checked ${ago(checked)}` : <span className="text-bad">not connected{o.broker.error ? `: ${o.broker.error}` : ''}</span>}
        </span>
      </div>

      <section className="grid grid-cols-1 gap-3 sm:grid-cols-3">
        <Card className="flex flex-col gap-1.5 px-[18px] py-4" tone={bad ? 'bad' : undefined}>
          <span className="text-[13px] text-muted">Working orders</span>
          <span className="flex items-center gap-2 text-xl font-semibold"><Dot tone={!checked ? 'off' : bad ? 'bad' : 'ok'} />
            {!checked ? `${o.orders.length} in the bot` : bad ? `${bad} differ` : <><span className="num">{o.orders.length} / {o.orders.length}</span> match</>}
          </span>
          <span className="text-xs text-muted">bot cycles vs broker, matched by orderRef</span>
        </Card>
        <Card className="flex flex-col gap-1.5 px-[18px] py-4" tone={o.position_mismatches.length ? 'bad' : undefined}>
          <span className="text-[13px] text-muted">Positions</span>
          <span className="flex items-center gap-2 text-xl font-semibold"><Dot tone={!checked ? 'off' : o.position_mismatches.length ? 'bad' : 'ok'} />
            {!checked ? 'broker not checked' : o.position_mismatches.length ? `${o.position_mismatches.length} mismatch${o.position_mismatches.length > 1 ? 'es' : ''}`
              : o.position_settling.length ? `${o.position_settling.length} rechecking` : 'all match'}
          </span>
          <span className="text-xs text-muted">
            {o.position_mismatches.length ? o.position_mismatches.map(m => `${m.symbol}: books ${qty(m.bot, false)}, broker ${qty(m.broker, false)}`).join(' · ')
              : o.position_settling.length ? 'differ in one snapshot; a mismatch only if the next one agrees' : `${o.positions.length} symbols held`}
          </span>
        </Card>
        <Card className="flex flex-col gap-1.5 px-[18px] py-4">
          <span className="text-[13px] text-muted">Executions today</span>
          <span className="num text-xl font-semibold">{o.executions.length}</span>
          <span className={`text-xs ${notBooked ? 'text-bad' : 'text-muted'}`}>
            {[notBooked ? `${notBooked} of the bot's not booked` : 'all of the bot\'s booked', manual > 0 && `${manual} manual`].filter(Boolean).join(' · ')}
          </span>
        </Card>
      </section>

      <Card>
        <div role="tablist" aria-label="View" className="flex flex-wrap gap-1 border-b border-line px-3 py-2.5">
          {tabs.map(t => (
            <button key={t.id} type="button" role="tab" aria-selected={tab === t.id} onClick={() => setTab(t.id)}
              className={`min-h-9 cursor-pointer rounded-md px-3.5 py-2 text-sm ${tab === t.id ? 'bg-sel text-text' : 'bg-transparent text-dim hover:bg-raised'}`}>
              {t.label} <span className="num text-muted">{t.count}</span>
            </button>
          ))}
        </div>

        {tab === 'orders' && (
          <>
            <ResponsiveTable label="Working orders" rows={o.orders} cols={ORDER_COLS} rowKey={r => r.client_order_id}
              rowClass={r => (r.match !== 'match' && r.match !== 'unknown' ? badRow : '')} />
            {!o.orders.length && <p className="m-0 px-4 py-6 text-sm text-muted">No orders working. Entries and exits go out at the first market event after a decision.</p>}
            {o.other_orders.length > 0 && (
              <p className="m-0 border-t border-line px-4 py-3 text-[13px] text-bad">
                {o.other_orders.length} other open order{o.other_orders.length > 1 ? 's' : ''} at the broker not placed by the bot: {o.other_orders.map(x => `${x.side} ${x.quantity} ${x.symbol} (${x.type}, ${x.status})`).join(' · ')}
              </p>
            )}
            <p className="m-0 px-4 py-3 text-[13px] text-muted">A row turns red when only one side has the order, or the quantities differ.</p>
          </>
        )}

        {tab === 'positions' && (
          <>
            <ResponsiveTable label="Positions" rows={o.positions} cols={POSITION_COLS} rowKey={p => p.symbol}
              rowClass={p => (p.match === false && !p.settling ? badRow : '')} />
            {!o.positions.length && <p className="m-0 px-4 py-6 text-sm text-muted">Nothing held, by the bot or at the broker.</p>}
          </>
        )}

        {tab === 'exec' && (
          o.executions.length
            ? <ResponsiveTable label="Executions today" rows={o.executions} cols={EXEC_COLS} rowKey={e => e.exec_id} />
            : <p className="m-0 px-4 py-6 text-sm text-muted">No executions yet today. MOC orders fill at the 16:00 ET auction (23:00 IL).</p>
        )}
      </Card>
    </>
  )
}
