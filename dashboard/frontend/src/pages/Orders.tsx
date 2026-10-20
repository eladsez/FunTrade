import { useState } from 'react'
import { Link } from 'react-router-dom'
import { useOrders } from '../lib/api'
import { qty } from '../lib/format'
import { ago } from '../lib/time'
import type { OrderRow } from '../lib/types'
import { Card, Dot, Failed, Loading, Td, Th, sideClass } from '../components/ui'

type Tab = 'orders' | 'positions' | 'exec'

const MATCH: Record<OrderRow['match'], [string, string]> = {
  match: ['✓', 'text-good'],
  missing_at_broker: ['not at broker', 'text-bad font-semibold'],
  differs: ['quantity/side differs', 'text-bad font-semibold'],
  unknown_to_bot: ['unknown to the bot', 'text-bad font-semibold'],
  unknown: ['broker not checked', 'text-muted'],
}

export default function Orders() {
  const { data: o, error, isLoading } = useOrders()
  const [tab, setTab] = useState<Tab>('orders')
  if (isLoading) return <Loading />
  if (error || !o) return <Failed error={error} />

  const checked = o.broker.checked_at ? o.broker.checked_at * 1000 : null
  const bad = o.orders.filter(r => r.match !== 'match' && r.match !== 'unknown').length
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
            {!checked ? 'broker not checked' : o.position_mismatches.length ? `${o.position_mismatches.length} mismatch${o.position_mismatches.length > 1 ? 'es' : ''}` : 'all match'}
          </span>
          <span className="text-xs text-muted">
            {o.position_mismatches.length ? o.position_mismatches.map(m => `${m.symbol}: books ${qty(m.bot, false)}, broker ${qty(m.broker, false)}`).join(' · ') : `${o.positions.length} symbols held`}
          </span>
        </Card>
        <Card className="flex flex-col gap-1.5 px-[18px] py-4">
          <span className="text-[13px] text-muted">Executions today</span>
          <span className="num text-xl font-semibold">{o.executions.length}</span>
          <span className="text-xs text-muted">{o.executions.filter(e => !e.booked).length ? `${o.executions.filter(e => !e.booked).length} not booked by the bot` : 'all booked'}</span>
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
          <div className="overflow-x-auto">
            <table className="w-full border-collapse">
              <thead><tr><Th>Basket</Th><Th>Leg</Th><Th>Side</Th><Th right>Qty</Th><Th>Type</Th><Th>Basket state</Th><Th>Broker status</Th><Th>orderRef</Th><Th>Match</Th></tr></thead>
              <tbody>
                {o.orders.map(r => (
                  <tr key={r.client_order_id} className={r.match !== 'match' && r.match !== 'unknown' ? 'bg-[#24141A]' : ''}>
                    <Td className="num text-[13px]"><Link to={`/baskets/${r.basket}`}>{r.basket}</Link></Td>
                    <Td className="num font-medium">{r.symbol}</Td>
                    <Td className={sideClass(r.side)}>{r.side}</Td>
                    <Td right className="num">{r.quantity ?? qty(r.broker?.quantity, false)}{r.broker && r.quantity != null && r.broker.quantity !== r.quantity && <span className="text-bad"> (broker {r.broker.quantity})</span>}</Td>
                    <Td className="num text-dim">{r.broker?.type ?? 'MOC'}</Td>
                    <Td className="num text-[13px]">{r.basket_state ?? '—'}</Td>
                    <Td className="num text-[13px]">{r.broker?.status ?? '—'}</Td>
                    <Td className="num text-xs text-muted">{r.client_order_id}</Td>
                    <Td className={`text-[13px] ${MATCH[r.match][1]}`}>{MATCH[r.match][0]}</Td>
                  </tr>
                ))}
              </tbody>
            </table>
            {!o.orders.length && <p className="m-0 px-4 py-6 text-sm text-muted">No orders working. Entries and exits go out at the first market event after a decision.</p>}
            {o.other_orders.length > 0 && (
              <p className="m-0 border-t border-line px-4 py-3 text-[13px] text-bad">
                {o.other_orders.length} other open order{o.other_orders.length > 1 ? 's' : ''} at the broker not placed by the bot: {o.other_orders.map(x => `${x.side} ${x.quantity} ${x.symbol} (${x.type}, ${x.status})`).join(' · ')}
              </p>
            )}
            <p className="m-0 px-4 py-3 text-[13px] text-muted">A row turns red when only one side has the order, or the quantities differ.</p>
          </div>
        )}

        {tab === 'positions' && (
          <div className="overflow-x-auto">
            <table className="w-full border-collapse">
              <thead><tr><Th>Symbol</Th><Th right>Bot books (all baskets)</Th><Th right>Broker</Th><Th>Baskets</Th><Th>Status</Th></tr></thead>
              <tbody>
                {o.positions.map(p => (
                  <tr key={p.symbol} className={p.match === false ? 'bg-[#24141A]' : ''}>
                    <Td className="num font-medium">{p.symbol}</Td>
                    <Td right className="num">{qty(p.bot)}</Td>
                    <Td right className="num">{qty(p.broker)}</Td>
                    <Td className="num text-[13px] text-dim">{Object.entries(p.baskets).map(([b, q]) => `${b} ${qty(q)}`).join(' · ') || '—'}</Td>
                    <Td className={`text-[13px] ${p.match == null ? 'text-muted' : p.match ? 'text-good' : 'font-semibold text-bad'}`}>{p.match == null ? 'broker not checked' : p.match ? 'Match' : 'Mismatch'}</Td>
                  </tr>
                ))}
              </tbody>
            </table>
            {!o.positions.length && <p className="m-0 px-4 py-6 text-sm text-muted">Nothing held, by the bot or at the broker.</p>}
          </div>
        )}

        {tab === 'exec' && (
          o.executions.length ? (
            <div className="overflow-x-auto">
              <table className="w-full border-collapse">
                <thead><tr><Th>Time (ET)</Th><Th>Symbol</Th><Th>Side</Th><Th right>Qty @ price</Th><Th>orderRef</Th><Th>Booked</Th></tr></thead>
                <tbody>
                  {o.executions.map(e => (
                    <tr key={e.exec_id}>
                      <Td className="num text-[13px] text-muted">{e.time.trim().split(/\s+/).pop()}</Td>
                      <Td className="num font-medium">{e.symbol}</Td>
                      <Td className={sideClass(e.side === 'BOT' ? 'BUY' : e.side === 'SLD' ? 'SELL' : e.side)}>{e.side === 'BOT' ? 'BUY' : e.side === 'SLD' ? 'SELL' : e.side}</Td>
                      <Td right className="num">{e.quantity} @ {e.price.toFixed(2)}</Td>
                      <Td className="num text-xs text-muted">{e.order_ref || '—'}</Td>
                      <Td className={`text-[13px] ${e.booked ? 'text-good' : 'font-semibold text-bad'}`}>{e.booked ? '✓' : 'not booked'}</Td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ) : <p className="m-0 px-4 py-6 text-sm text-muted">No executions yet today. MOC orders fill at the 16:00 ET auction (23:00 IL).</p>
        )}
      </Card>
    </>
  )
}
