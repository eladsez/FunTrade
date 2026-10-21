import { Link } from 'react-router-dom'
import { useOverview, useSession } from '../lib/api'
import { bps, money, pnlClass, qty, usd } from '../lib/format'
import { fmtDay, fmtWhen, pricesNote, sessionView, useNow } from '../lib/time'
import type { Fill, FillQuality } from '../lib/types'
import { Banner, Card, CardHead, Dot, Failed, Loading, Tile, sideClass } from '../components/ui'
import { BasketList } from '../components/BasketList'
import { ResponsiveTable, type Col } from '../components/ResponsiveTable'

export function SessionLine() {
  const now = useNow(30_000)
  const { data } = useSession()
  if (!data) return <section aria-label="Session" className="text-[13px] text-muted">Market calendar…</section>
  const s = sessionView(data, now)
  const closedWhy = s.holiday ? `US market closed: ${s.holiday}` : 'US market closed'
  return (
    <section aria-label="Session" className="flex flex-wrap items-center gap-x-7 gap-y-1 text-[13px] text-dim md:text-sm">
      <span className="flex items-center gap-2"><Dot tone={s.open ? 'ok' : 'off'} /><b className="font-semibold text-text">{s.open ? 'US session open' : closedWhy}</b></span>
      {s.open && s.cutoff.in && <span>MOC cutoff <span className="num">{s.cutoff.ny} ET · {s.cutoff.il} IL</span> <span className="text-muted">in {s.cutoff.in}</span></span>}
      {s.open && <span>Close <span className="num">{s.close.ny} ET · {s.close.il} IL</span> <span className="text-muted">in {s.close.in}</span></span>}
      {!s.open && <span>Opens{s.opensOn && ` ${s.opensOn}`} <span className="num">{s.opens.ny} ET · {s.opens.il} IL</span></span>}
      {s.earlyClose && <span className="text-warn">Early close {s.close.ny} ET (MOC cutoff {s.cutoff.ny})</span>}
      <span className="hidden md:inline">Decisions after the EOD fetch <span className="num">{s.eod.ny} ET · {s.eod.il} IL</span></span>
      {!s.known && <span className="text-warn">Holiday calendar not loaded for this year</span>}
    </section>
  )
}

const FILL_COLS: Col<Fill>[] = [
  { key: 'when', head: 'When (ET)', cell: f => fmtWhen(f.t), className: 'num text-[13px] text-muted', card: 'body' },
  { key: 'basket', head: 'Basket', cell: f => <Link to={`/baskets/${f.basket}`}>{f.basket}</Link>, className: 'num text-[13px]', card: 'title' },
  { key: 'side', head: 'Side', cell: f => `${f.side} ${f.symbol}`, className: f => sideClass(f.side), card: 'title' },
  { key: 'qty', head: 'Qty @ price', right: true, cell: f => `${f.quantity} @ ${f.price.toFixed(2)}`, className: 'num', card: 'body' },
  { key: 'vs', head: 'vs close', right: true, cell: f => (f.cost_bps == null ? 'close pending' : bps(f.cost_bps)),
    className: f => `num text-[13px] ${f.cost_bps == null ? 'text-muted' : f.cost_bps > 0 ? 'text-bad' : 'text-good'}`, card: 'aside' },
]

function PaperVsResearch({ q, commissions, commissionFills }: { q: FillQuality; commissions: number | null; commissionFills: number }) {
  return (
    <Card>
      <CardHead title="Paper vs research" />
      <div className="grid grid-cols-2 gap-3.5 px-4 py-3.5 sm:grid-cols-4">
        <div><div className="text-xs text-muted">Fills vs the close</div><div className="num mt-0.5 text-base">{q.fills}{q.pending > 0 && <span className="text-[13px] text-muted"> + {q.pending} pending</span>}</div></div>
        <div><div className="text-xs text-muted">Avg vs close</div><div className={`num mt-0.5 text-base ${q.bps == null ? '' : q.bps > 0 ? 'text-bad' : 'text-good'}`}>{bps(q.bps)}</div></div>
        <div><div className="text-xs text-muted">Cost vs close</div><div className="num mt-0.5 text-base">{usd(q.cost_usd, { sign: true, cents: true })}</div></div>
        <div><div className="text-xs text-muted">Commissions</div><div className="num mt-0.5 text-base">{usd(commissions, { cents: true })}<span className="text-[13px] text-muted"> {commissionFills} fills</span></div></div>
      </div>
      <p className="m-0 border-t border-line px-4 py-3 text-[13px] leading-normal text-muted">
        The research assumes every fill at the official close. Paper MOC orders fill at ~15:59:30 ET at the last price, so a gap is expected
        {q.worst && <> (worst: <span className="num">{q.worst.symbol}</span> in {q.worst.basket}, {bps(q.worst.cost_bps)})</>}.
        Positive = worse than the close. A fill's close is known after the bot's EOD fetch (18:00 ET).
      </p>
    </Card>
  )
}

export default function Overview() {
  const { data: o, error, isLoading } = useOverview()
  if (isLoading) return <Loading />
  if (error || !o) return <Failed error={error} />

  const grossPct = o.gross_exposure_usd != null && o.gross_cap_usd ? (o.gross_exposure_usd + (o.reserved_gross_usd ?? 0)) / o.gross_cap_usd : null
  const pnlOpen = o.unrealized
  const controlsOn = o.flags.kill || o.flags.flatten

  return (
    <>
      <SessionLine />

      {(o.halted.length > 0 || o.mismatches.length > 0 || o.settling.length > 0 || o.bot.state !== 'running' || o.working_orders > 0) && (
        <section aria-label="Alerts" className="flex flex-col gap-2">
          {o.bot.state !== 'running' && (
            <Banner tone="bad" to="/system" action="System">
              <b className="text-bad">Bot {o.bot.state}</b>
              {o.bot.age_s != null && <> · last heartbeat {Math.round(o.bot.age_s / 60)} min ago</>}
              {o.bot.state === 'unknown' && <> · no status file (a bot version without the journal?)</>}
            </Banner>
          )}
          {o.halted.map(h => (
            <Banner key={h.id} tone="bad" to="/system" action="How to clear">
              <b className="text-bad">{h.id} halted</b> · {h.reason ?? 'no reason recorded'}
            </Banner>
          ))}
          {o.mismatches.map(m => (
            <Banner key={m.symbol} tone="bad" to="/orders" action="Positions">
              <b className="text-bad">{m.symbol}: books {qty(m.bot, false)}, broker {qty(m.broker, false)}</b>
            </Banner>
          ))}
          {o.settling.length > 0 && (
            <Banner tone="info" to="/orders" action="Positions">
              Rechecking {o.settling.map(m => m.symbol).join(', ')}: books and broker differ in one snapshot (normal right after fills)
            </Banner>
          )}
          {o.working_orders > 0 && (
            <Banner tone="info" to="/orders" action="Orders">
              {o.working_orders} MOC orders working (fill at today's close)
            </Banner>
          )}
        </section>
      )}

      <section aria-label="Account" className="grid grid-cols-2 gap-2.5 md:grid-cols-[repeat(auto-fit,minmax(220px,1fr))] md:gap-3">
        <Tile label="Equity (net liquidation)" sub={o.net_liquidation ? money(o.net_liquidation.value, o.net_liquidation.currency) : 'USD, from the bot'}>
          {usd(o.equity_usd)}
        </Tile>
        <Tile label="Open P&L (books)" sub={[`${o.in_position} basket${o.in_position === 1 ? '' : 's'} holding`, pricesNote(o.prices)].filter(Boolean).join(' · ')}>
          <span className={pnlClass(pnlOpen)}>{pnlOpen == null && o.in_position ? '—' : usd(pnlOpen ?? 0, { sign: true })}</span>
        </Tile>
        <Tile label="Realized P&L" sub={[o.first_run ? `since ${fmtDay(new Date(o.first_run).toISOString().slice(0, 10))}` : 'all time',
          o.commissions != null && `commissions ${usd(o.commissions, { cents: true })}`].filter(Boolean).join(' · ')}>
          <span className={pnlClass(o.realized)}>{usd(o.realized, { sign: true })}</span>
        </Tile>
        <div className="flex flex-col gap-2 rounded-[10px] border border-line bg-panel px-3.5 py-3 md:px-[18px] md:py-4">
          <span className="text-xs text-muted md:text-[13px]">Gross exposure</span>
          <span className="num text-xl font-semibold md:text-[26px]">
            {grossPct != null ? `${Math.round(grossPct * 100)}%` : '—'}
            <span className="ml-2 hidden text-sm font-normal text-muted md:inline">{usd(o.gross_exposure_usd)} / {usd(o.gross_cap_usd)}</span>
          </span>
          <div className="h-1.5 overflow-hidden rounded-[3px] bg-line"><div className="h-full bg-accent" style={{ width: `${Math.min(100, (grossPct ?? 0) * 100)}%` }} /></div>
          {!!o.reserved_gross_usd && <span className="text-[11px] text-muted md:text-xs">incl. {usd(o.reserved_gross_usd)} reserved for working entries</span>}
        </div>
        <Link to="/system" className="col-span-2 flex flex-col gap-1.5 rounded-[10px] border border-line bg-panel px-3.5 py-3 text-text md:col-span-1 md:px-[18px] md:py-4">
          <span className="text-xs text-muted md:text-[13px]">Controls</span>
          <span className="flex items-center gap-2 text-lg font-semibold"><Dot tone={o.flags.flatten ? 'bad' : o.flags.kill ? 'warn' : 'ok'} />
            {o.flags.flatten ? 'Flattening' : o.flags.kill ? 'Kill switch on' : 'Trading normally'}</span>
          <span className="text-xs text-muted">KILL {o.flags.kill ? 'on' : 'off'} · FLATTEN {o.flags.flatten ? 'on' : 'off'}</span>
          {!controlsOn && <span className="text-[13px] text-link">Open controls →</span>}
        </Link>
      </section>

      <Card>
        <BasketList rows={o.baskets} initial="active" limit={12} />
      </Card>

      <section className="grid grid-cols-[minmax(0,1fr)] gap-4 md:grid-cols-[repeat(auto-fit,minmax(380px,1fr))]">
        <Card>
          <CardHead title="Today" />
          {o.timeline.length ? (
            <ol className="m-0 flex list-none flex-col gap-2.5 px-4 py-3.5 text-sm">
              {o.timeline.map((t, i) => (
                <li key={i} className="flex items-baseline gap-3.5">
                  <span className="num shrink-0 text-[13px] text-muted"><span className="hidden md:inline">{fmtWhen(t.t, 'NY')} ET · </span>{fmtWhen(t.t, 'IL')} IL</span>
                  <span className={t.type === 'halt' ? 'text-bad' : t.type === 'entry_skipped' || t.type === 'stop' ? 'text-muted' : 'text-text'}>{t.text}</span>
                </li>
              ))}
            </ol>
          ) : <p className="m-0 px-4 py-3.5 text-sm text-muted">Nothing journaled yet today (New York time).</p>}
        </Card>
        <Card>
          <CardHead title="Recent fills" />
          {o.fills.length
            ? <ResponsiveTable label="Recent fills" rows={o.fills} cols={FILL_COLS} rowKey={(f, i) => f.exec_id ?? String(i)} />
            : <p className="m-0 px-4 py-3.5 text-sm text-muted">No fills journaled yet.</p>}
        </Card>
      </section>

      <PaperVsResearch q={o.fill_quality} commissions={o.commissions} commissionFills={o.commission_fills} />
    </>
  )
}
