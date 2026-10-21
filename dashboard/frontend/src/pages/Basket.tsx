import { useState, type ReactNode } from 'react'
import { Link, useParams } from 'react-router-dom'
import { ChevronLeft } from 'lucide-react'
import { useBasket, useSession } from '../lib/api'
import { useStream } from '../lib/live'
import { bps, held, pnlClass, price, qty, usd, z as fmtZ } from '../lib/format'
import { fmtDay, fmtWhen, priceNote, sessionView } from '../lib/time'
import type { BasketDetail, Cycle, Fill, Leg } from '../lib/types'
import { Card, CardHead, Failed, Loading, Pills, StateChip, sideClass } from '../components/ui'
import { ResponsiveTable, type Col } from '../components/ResponsiveTable'
import { ZChart, ZChartLegend } from '../components/ZChart'
import { note } from '../components/BasketList'

type Range = '30' | '90' | '250' | 'all'

function Stat({ label, children, sub, className = '' }: { label: string; children: ReactNode; sub?: ReactNode; className?: string }) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-[11px] text-muted md:text-xs">{label}</span>
      <span className={`num text-[17px] font-semibold md:text-[22px] ${className}`}>{children}</span>
      {sub && <span className="text-[11px] text-muted">{sub}</span>}
    </div>
  )
}

const legPnl = (l: Leg) => (Math.abs(l.quantity) > 1e-9 ? (l.unrealized == null ? null : l.unrealized + l.realized) : l.realized)

const brokerText = (l: Leg) => (l.broker_match == null ? 'unknown' : l.broker_match ? 'matches' : l.broker_settling ? 'rechecking' : 'MISMATCH')
const brokerClass = (l: Leg) => (l.broker_match == null ? 'text-muted' : l.broker_match ? 'text-good' : l.broker_settling ? 'text-warn' : 'font-semibold text-bad')

const LEG_COLS: Col<Leg>[] = [
  { key: 'sym', head: 'Symbol', cell: l => l.symbol, className: 'num font-medium', card: 'title' },
  { key: 'w', head: 'Weight', right: true, cell: l => qty(l.weight), className: 'num text-dim' },
  { key: 'q', head: 'Shares', right: true, cell: l => qty(l.quantity), card: 'title',
    className: l => `num ${l.quantity < 0 ? 'text-short' : l.quantity > 0 ? 'text-long' : 'text-muted'}` },
  { key: 'avg', head: 'Avg', right: true, cell: l => (l.quantity ? price(l.avg_price) : '—'), className: 'num text-dim' },
  { key: 'px', head: 'Price', right: true, className: 'num',
    cell: l => <>{price(l.price)}<div className="text-[11px] font-normal text-muted">{priceNote(l)}</div></> },
  { key: 'pnl', head: 'P&L', right: true, cell: l => usd(legPnl(l), { sign: true }), className: l => `num ${pnlClass(legPnl(l))}`, card: 'aside' },
  { key: 'broker', head: 'Broker', cell: brokerText, className: l => `text-[13px] ${brokerClass(l)}` },
]

const fillText = (f: Fill) => `${f.side} ${f.quantity} ${f.symbol} @ ${f.price.toFixed(2)}`
  + (f.cost_bps != null ? ` (close ${price(f.close)}, ${bps(f.cost_bps)})` : '')

const CYCLE_COLS: Col<Cycle>[] = [
  { key: 'sent', head: 'Sent', cell: c => fmtDay(c.day), className: 'num', card: 'title' },
  { key: 'kind', head: 'Kind', cell: c => c.label, card: 'title' },
  { key: 'side', head: 'Side', cell: c => c.position, className: c => sideClass(c.position), card: 'title' },
  { key: 'res', head: 'Resolved', cell: c => (c.end_t ? fmtWhen(c.end_t) : 'working'), className: 'num text-dim' },
  { key: 'result', head: 'Result', cell: c => (c.end_state ? <StateChip state={c.end_state} small /> : '—'), card: 'aside' },
  { key: 'fills', head: 'Fills (vs the close)', label: 'Fills', className: 'num text-[13px] whitespace-normal text-dim',
    cell: c => (c.fills.length ? c.fills.map(fillText).join(' · ') : c.end_t ? 'no fills' : '—') },
]

function Next({ b }: { b: BasketDetail }) {
  const { data } = useSession()
  const s = data ? sessionView(data) : null
  const close = s ? `Close ${s.close.ny} ET` : 'Next close'
  const rows: [string, string][] = []
  if (b.state === 'IN_POSITION') {
    const cond = b.position === 'LONG' ? `z is above ${fmtZ(-b.exit_z)}` : `z is below ${fmtZ(b.exit_z)}`
    rows.push([close, `If ${cond}, the exit is decided.`])
    rows.push(['Next session', 'Exit goes out as MOC orders, filling at that day’s close.'])
    rows.push(['Otherwise', `Holds. No stop on z${b.loss_limit ? `; the basket loss limit is ${usd(b.loss_limit)}` : ''}.`])
  } else if (b.state === 'FLAT') {
    rows.push([close, `Enters long if z < −${b.entry_z.toFixed(2)}, short if z > +${b.entry_z.toFixed(2)}.`])
    rows.push(['Next session', 'An entry goes out as MOC orders, sized with that close.'])
  } else if (b.state === 'ENTRY_PENDING' || b.state === 'EXIT_PENDING') {
    rows.push([close, `${b.working_legs} MOC orders fill in the closing auction.`])
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
  const net = pnl == null || b.commissions == null ? null : pnl - b.commissions
  const e = b.entry
  const q = b.fill_quality
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
          <Stat label="Entered at" sub={e ? `z of ${e.decision_day ? fmtDay(e.decision_day) : '?'} · in ${fmtDay(e.day)}${e.restored ? ' · restored' : ''}` : undefined}>
            {e ? fmtZ(e.z) : '—'}
          </Stat>
          <Stat label="Held" sub={b.held_days != null ? 'trading days' : undefined}>{held(b.held_days, true)}</Stat>
          <Stat label="P&L" className={pnlClass(pnl)} sub={b.commissions == null ? undefined
            : net == null ? `fees ${usd(b.commissions, { cents: true })}` : `${usd(net, { sign: true })} after ${usd(b.commissions, { cents: true })} fees`}>
            {usd(pnl, { sign: true })}
          </Stat>
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
          <ResponsiveTable label="Legs" rows={b.legs} cols={LEG_COLS} rowKey={l => l.symbol} />
          <p className="m-0 px-4 py-3 text-[13px] leading-normal text-muted">
            Long the spread = buy the positive-weight legs, sell the negative ones. "Broker" compares the bot's books for the symbol (all baskets) with IBKR's position.
            Prices are the day's close once the bot has it (18:00 ET), otherwise IBKR delayed quotes (~15 min behind in session).
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
        <CardHead title="Trades (cycles)">
          {q.fills > 0 && (
            <span className="text-[13px] text-muted">
              fills vs the close: <span className={`num ${q.bps != null && q.bps > 0 ? 'text-bad' : 'text-good'}`}>{bps(q.bps)}</span> avg,{' '}
              <span className="num">{usd(q.cost_usd, { sign: true, cents: true })}</span> over {q.fills}{q.pending > 0 && ` (+${q.pending} pending)`}
            </span>
          )}
        </CardHead>
        {b.cycles.length
          ? <ResponsiveTable label="Trades" rows={[...b.cycles].reverse()} cols={CYCLE_COLS} rowKey={c => String(c.t)} />
          : <p className="m-0 px-4 py-3.5 text-sm text-muted">No trades journaled for this basket yet.</p>}
        {b.skipped.length > 0 && (
          <p className="m-0 border-t border-line px-4 py-3 text-[13px] text-muted">
            Last skipped entry: {fmtWhen(b.skipped[b.skipped.length - 1].t)} ET, {b.skipped[b.skipped.length - 1].reason}
          </p>
        )}
      </Card>
    </>
  )
}
