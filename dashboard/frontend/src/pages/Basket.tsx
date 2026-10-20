import { useState, type ReactNode } from 'react'
import { Link, useParams } from 'react-router-dom'
import { ChevronLeft } from 'lucide-react'
import { useBasket } from '../lib/api'
import { useStream } from '../lib/live'
import { pnlClass, price, qty, usd, z as fmtZ } from '../lib/format'
import { fmtDay, fmtWhen, session } from '../lib/time'
import type { BasketDetail } from '../lib/types'
import { Card, CardHead, Failed, Loading, Pills, StateChip, Td, Th, sideClass } from '../components/ui'
import { ZChart, ZChartLegend } from '../components/ZChart'
import { note } from '../components/BasketList'

type Range = '30' | '90' | '250' | 'all'

function Stat({ label, children, className = '' }: { label: string; children: ReactNode; className?: string }) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-[11px] text-muted md:text-xs">{label}</span>
      <span className={`num text-[17px] font-semibold md:text-[22px] ${className}`}>{children}</span>
    </div>
  )
}

function Next({ b }: { b: BasketDetail }) {
  const s = session()
  const rows: [string, string][] = []
  if (b.state === 'IN_POSITION') {
    const cond = b.position === 'LONG' ? `z is above ${fmtZ(-b.exit_z)}` : `z is below ${fmtZ(b.exit_z)}`
    rows.push([`Close ${s.close.ny} ET`, `If ${cond}, the exit is decided.`])
    rows.push(['Next session', 'Exit goes out as MOC orders, filling at that day’s close.'])
    rows.push(['Otherwise', `Holds. No stop on z${b.loss_limit ? `; the basket loss limit is ${usd(b.loss_limit)}` : ''}.`])
  } else if (b.state === 'FLAT') {
    rows.push([`Close ${s.close.ny} ET`, `Enters long if z < −${b.entry_z.toFixed(2)}, short if z > +${b.entry_z.toFixed(2)}.`])
    rows.push(['Next session', 'An entry goes out as MOC orders, sized with that close.'])
  } else if (b.state === 'ENTRY_PENDING' || b.state === 'EXIT_PENDING') {
    rows.push([`Close ${s.close.ny} ET`, `${b.working_legs} MOC orders fill in the closing auction.`])
    rows.push(['After the fills', b.state === 'ENTRY_PENDING' ? 'IN_POSITION if every leg filled; a partial fill triggers a recovery sweep.' : 'FLAT once every leg filled.'])
  } else if (b.state === 'SYSTEM_HALTED') {
    rows.push(['Halted', b.halt_reason ?? 'needs a human'])
    rows.push(['To resume', 'See System → Halted baskets.'])
  } else {
    rows.push(['Recovery', 'The unbalanced legs are flattened at the next session.'])
  }
  return (
    <div className="flex flex-col gap-3.5 p-4 text-sm leading-normal">
      {rows.map(([k, v]) => (
        <div key={k} className="flex gap-3"><span className="num w-[120px] shrink-0 text-muted">{k}</span><span>{v}</span></div>
      ))}
    </div>
  )
}

export default function Basket() {
  const { id = '' } = useParams()
  const { data: b, error, isLoading } = useBasket(id)
  const { live } = useStream()
  const [range, setRange] = useState<Range>('90')
  if (isLoading) return <Loading />
  if (error || !b) return <Failed error={error} />

  const closes = range === 'all' ? b.closes : b.closes.slice(-Number(range))
  const inBooks = b.legs.some(l => Math.abs(l.quantity) > 1e-9)
  const unreal = b.legs.every(l => l.unrealized != null) ? b.legs.reduce((s, l) => s + (l.unrealized ?? 0), 0) : null
  const pnl = inBooks ? (unreal == null ? null : unreal + b.realized) : b.realized
  const entry = [...b.cycles].reverse().find(c => c.label === 'ENTRY')
  const entryClose = entry ? b.closes.filter(c => c.day < entry.day && (c.action === 'ENTER_LONG' || c.action === 'ENTER_SHORT')).pop() : undefined
  const intraday = live?.baskets[b.id]?.z
  const m = b.metrics

  return (
    <>
      <div className="flex flex-wrap items-end gap-x-6 gap-y-3">
        <div className="flex flex-col gap-1.5">
          <Link to="/" className="flex items-center gap-1 text-[13px]"><ChevronLeft size={16} aria-hidden />All baskets</Link>
          <h1 className="num m-0 text-[22px] font-semibold md:text-[26px]">{b.id}</h1>
          <span className="text-sm text-dim">{b.tickers.join(' · ')}</span>
        </div>
        <div className="flex flex-wrap gap-2">
          <StateChip state={b.state} />
          {b.position !== 'FLAT' && <span className={`rounded border px-2.5 py-1 text-xs font-semibold ${b.position === 'LONG' ? 'border-long-bd bg-long-bg text-long' : 'border-short-bd bg-short-bg text-short'}`}>{b.position} the spread</span>}
        </div>
        <div className="grid w-full grid-cols-4 gap-2 md:ml-auto md:w-auto md:gap-7">
          <Stat label="z last close">{fmtZ(b.close_z)}</Stat>
          <Stat label="Entered at">{entryClose ? fmtZ(entryClose.z) : '—'}</Stat>
          <Stat label="Held">{b.held_days != null ? `${b.held_days} d` : '—'}</Stat>
          <Stat label="P&L" className={pnlClass(pnl)}>{usd(pnl, { sign: true })}</Stat>
        </div>
      </div>

      <Card className="flex flex-col gap-3 p-3 md:px-[18px] md:py-4">
        <div className="flex flex-wrap items-center gap-3">
          <h2 className="m-0 text-base font-semibold">z-score, daily closes</h2>
          <span className="text-[13px] text-muted">window {b.window} days · enter beyond ±{b.entry_z.toFixed(2)} · exit {b.exit_z < 0 ? `once z crosses ${fmtZ(-b.exit_z)} / ${fmtZ(b.exit_z)}` : `inside ±${b.exit_z.toFixed(2)}`}</span>
          <div className="ml-auto">
            <Pills label="Range" value={range} onChange={setRange} options={[{ id: '30', label: '30d' }, { id: '90', label: '90d' }, { id: '250', label: '1y' }, { id: 'all', label: 'All' }]} />
          </div>
        </div>
        <div className="md:hidden"><ZChart closes={closes} entry={b.entry_z} exit={b.exit_z} height={170} /></div>
        <div className="hidden md:block"><ZChart closes={closes} entry={b.entry_z} exit={b.exit_z} /></div>
        <div className="flex flex-wrap items-center gap-4">
          <ZChartLegend />
          {intraday != null && <span className="text-[13px] text-muted">Intraday z (display only, not traded): <span className="num">{fmtZ(intraday)}</span></span>}
        </div>
      </Card>

      <section className="grid grid-cols-[minmax(0,1fr)] gap-4 lg:grid-cols-2">
        <Card>
          <CardHead title="Legs" />
          <div className="overflow-x-auto">
            <table className="w-full border-collapse">
              <thead><tr><Th>Symbol</Th><Th right>Weight</Th><Th right>Shares</Th><Th right>Avg</Th><Th right>Last</Th><Th right>P&amp;L</Th><Th>Broker</Th></tr></thead>
              <tbody>
                {b.legs.map(l => {
                  const lp = Math.abs(l.quantity) > 1e-9 ? (l.unrealized == null ? null : l.unrealized + l.realized) : l.realized
                  return (
                    <tr key={l.symbol}>
                      <Td className="num font-medium">{l.symbol}</Td>
                      <Td right className="num text-dim">{qty(l.weight)}</Td>
                      <Td right className={`num ${l.quantity < 0 ? 'text-short' : l.quantity > 0 ? 'text-long' : 'text-muted'}`}>{qty(l.quantity)}</Td>
                      <Td right className="num text-dim">{l.quantity ? price(l.avg_price) : '—'}</Td>
                      <Td right className="num">{price(l.price)}</Td>
                      <Td right className={`num ${pnlClass(lp)}`}>{usd(lp, { sign: true })}</Td>
                      <Td className={`text-[13px] ${l.broker_match == null ? 'text-muted' : l.broker_match ? 'text-good' : 'font-semibold text-bad'}`}>
                        {l.broker_match == null ? 'unknown' : l.broker_match ? 'matches' : 'MISMATCH'}
                      </Td>
                    </tr>
                  )
                })}
              </tbody>
            </table>
          </div>
          <p className="m-0 px-4 py-3 text-[13px] leading-normal text-muted">
            Long the spread = buy the positive-weight legs, sell the negative ones. "Broker" compares the bot's books for the symbol (all baskets) with IBKR's position.
          </p>
        </Card>

        <Card className="flex flex-col">
          <CardHead title="What happens next" />
          {note(b) && <p className="m-0 px-4 pt-3.5 text-sm text-dim">{note(b)}</p>}
          <Next b={b} />
          <h3 className="m-0 border-t border-line px-4 pt-3 pb-2 text-[13px] font-semibold text-muted">Research{b.exported_at && ` (export ${b.exported_at.slice(0, 10)})`}</h3>
          <dl className="m-0 grid grid-cols-2 gap-3 px-4 pb-4 sm:grid-cols-4">
            {[['OOS Sharpe', m.oos_sharpe?.toFixed(2)], ['Half-life', m.train_hl != null ? `${m.train_hl.toFixed(1)} d` : null],
              ['Train p', m.train_p?.toFixed(4)], ['OOS p', m.oos_p?.toFixed(4)]].map(([k, v]) => (
              <div key={k}><dt className="text-xs text-muted">{k}</dt><dd className="num m-0 mt-0.5 text-base">{v ?? '—'}</dd></div>
            ))}
          </dl>
        </Card>
      </section>

      <Card>
        <CardHead title="Trades (cycles)" />
        {b.cycles.length ? (
          <div className="overflow-x-auto">
            <table className="w-full border-collapse">
              <thead><tr><Th>Sent</Th><Th>Kind</Th><Th>Side</Th><Th>Resolved</Th><Th>Result</Th><Th>Fills</Th></tr></thead>
              <tbody>
                {[...b.cycles].reverse().map((c, i) => (
                  <tr key={i}>
                    <Td className="num">{fmtDay(c.day)}</Td>
                    <Td>{c.label}</Td>
                    <Td className={sideClass(c.position)}>{c.position}</Td>
                    <Td className="num text-dim">{c.end_t ? fmtWhen(c.end_t) : 'working'}</Td>
                    <Td>{c.end_state ? <StateChip state={c.end_state} small /> : '—'}</Td>
                    <Td className="num text-[13px] whitespace-normal text-dim">
                      {c.fills.length ? c.fills.map(f => `${f.side} ${f.quantity} ${f.symbol} @ ${f.price.toFixed(2)}`).join(' · ') : c.end_t ? 'no fills' : '—'}
                    </Td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        ) : <p className="m-0 px-4 py-3.5 text-sm text-muted">No trades journaled for this basket yet.</p>}
        {b.skipped.length > 0 && (
          <p className="m-0 border-t border-line px-4 py-3 text-[13px] text-muted">
            Last skipped entry: {fmtWhen(b.skipped[b.skipped.length - 1].t)} ET, {b.skipped[b.skipped.length - 1].reason}
          </p>
        )}
      </Card>
    </>
  )
}
