import { Link } from 'react-router-dom'
import { useOverview } from '../lib/api'
import { money, pnlClass, usd } from '../lib/format'
import { fmtDay, fmtWhen, session, useNow } from '../lib/time'
import { Banner, Card, CardHead, Dot, Failed, Loading, Td, Th, Tile, sideClass } from '../components/ui'
import { BasketList } from '../components/BasketList'

export function SessionLine() {
  useNow(30_000)
  const s = session()
  return (
    <section aria-label="Session" className="flex flex-wrap items-center gap-x-7 gap-y-1 text-[13px] text-dim md:text-sm">
      <span className="flex items-center gap-2"><Dot tone={s.open ? 'ok' : 'off'} /><b className="font-semibold text-text">{s.open ? 'US session open' : 'US market closed'}</b></span>
      {s.open && s.cutoff.in && <span>MOC cutoff <span className="num">{s.cutoff.ny} ET · {s.cutoff.il} IL</span> <span className="text-muted">in {s.cutoff.in}</span></span>}
      {s.open && <span>Close <span className="num">{s.close.ny} ET · {s.close.il} IL</span> <span className="text-muted">in {s.close.in}</span></span>}
      {!s.open && <span>Opens <span className="num">{s.opens.ny} ET · {s.opens.il} IL</span>{!s.weekday && ' on Monday'}</span>}
      <span className="hidden md:inline">Decisions after the EOD fetch <span className="num">{s.eod.ny} ET · {s.eod.il} IL</span></span>
    </section>
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

      {(o.halted.length > 0 || o.mismatches.length > 0 || o.bot.state !== 'running' || o.working_orders > 0) && (
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
              <b className="text-bad">{m.symbol}: books {m.bot}, broker {m.broker}</b>
            </Banner>
          ))}
          {o.working_orders > 0 && (
            <Banner tone="info" to="/orders" action="Orders">
              {o.working_orders} MOC orders working (fill at the {session().close.ny} ET close)
            </Banner>
          )}
        </section>
      )}

      <section aria-label="Account" className="grid grid-cols-2 gap-2.5 md:grid-cols-[repeat(auto-fit,minmax(220px,1fr))] md:gap-3">
        <Tile label="Equity (net liquidation)" sub={o.net_liquidation ? money(o.net_liquidation.value, o.net_liquidation.currency) : 'USD, from the bot'}>
          {usd(o.equity_usd)}
        </Tile>
        <Tile label="Open P&L (books)" sub={`${o.in_position} basket${o.in_position === 1 ? '' : 's'} holding`}>
          <span className={pnlClass(pnlOpen)}>{pnlOpen == null && o.in_position ? '—' : usd(pnlOpen ?? 0, { sign: true })}</span>
        </Tile>
        <Tile label="Realized P&L" sub={o.first_run ? `since ${fmtDay(new Date(o.first_run).toISOString().slice(0, 10))}` : 'all time'}>
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
          {o.fills.length ? (
            <div className="overflow-x-auto">
              <table className="w-full border-collapse">
                <thead><tr><Th>When (ET)</Th><Th>Basket</Th><Th>Side</Th><Th right>Qty @ price</Th></tr></thead>
                <tbody>
                  {o.fills.map((f, i) => (
                    <tr key={i}>
                      <Td className="num text-[13px] text-muted">{fmtWhen(f.t)}</Td>
                      <Td className="num text-[13px]"><Link to={`/baskets/${f.basket}`}>{f.basket}</Link></Td>
                      <Td className={sideClass(f.side)}>{f.side} {f.symbol}</Td>
                      <Td right className="num">{f.quantity} @ {f.price.toFixed(2)}</Td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          ) : <p className="m-0 px-4 py-3.5 text-sm text-muted">No fills journaled yet.</p>}
        </Card>
      </section>
    </>
  )
}
