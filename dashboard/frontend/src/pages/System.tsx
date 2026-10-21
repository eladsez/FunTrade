import { useState, type ReactNode } from 'react'
import { Link } from 'react-router-dom'
import { useClearHalt, useSetFlag, useSystem } from '../lib/api'
import { qty, usd } from '../lib/format'
import { ago, fmtWhen, outage } from '../lib/time'
import type { Halt, LogRun, SymbolCap, System as Sys } from '../lib/types'
import { Card, CardHead, Dot, Failed, Loading } from '../components/ui'
import { Confirm } from '../components/Confirm'

type Ask = { kind: 'kill' | 'flatten'; on: boolean } | { kind: 'halt'; halt: Halt } | null

function Control({ name, on, children, button, tone, onClick }: {
  name: string; on: boolean; children: ReactNode; button: string; tone: 'warn' | 'bad'; onClick: () => void
}) {
  const cls = tone === 'bad' ? 'border-bad-bd bg-bad-bg text-bad' : 'border-short-bd bg-short-bg text-short'
  return (
    <div className="flex flex-wrap items-center gap-4">
      <div className="flex min-w-[240px] flex-1 flex-col gap-1">
        <span className="text-[15px] font-semibold">{name}
          <span className={`ml-2 text-xs font-medium ${on ? (tone === 'bad' ? 'text-bad' : 'text-short') : 'text-good'}`}>{on ? 'ON' : 'OFF'}</span>
        </span>
        <span className="text-[13px] leading-normal text-muted">{children}</span>
      </div>
      <button type="button" onClick={onClick} className={`min-h-12 w-full cursor-pointer rounded-lg border px-[18px] text-sm font-semibold sm:w-auto ${on ? 'border-line bg-raised text-text' : cls}`}>{button}</button>
    </div>
  )
}

function healthRows(s: Sys): { k: string; v: string; sub?: string; tone: 'ok' | 'warn' | 'bad' | 'off' }[] {
  const bot = s.bot
  const botTone = bot.state === 'running' ? 'ok' : bot.state === 'stale' ? 'warn' : bot.state === 'stopped' ? 'warn' : 'bad'
  return [
    { k: 'Bot', v: bot.state === 'unknown' ? 'No status file' : bot.state[0].toUpperCase() + bot.state.slice(1),
      sub: [bot.started_at && `since ${fmtWhen(bot.started_at)} ET`, bot.git_sha && `image ${bot.git_sha.slice(0, 7)}`, bot.age_s != null && `heartbeat ${ago(Date.now() - bot.age_s * 1000)}`].filter(Boolean).join(' · '), tone: botTone },
    { k: 'Bot ↔ Gateway', v: bot.broker_connected == null ? 'unknown' : bot.broker_connected ? 'Connected' : 'Disconnected', tone: bot.broker_connected ? 'ok' : bot.broker_connected == null ? 'off' : 'bad' },
    { k: 'Gateway ↔ IBKR', v: s.ibkr.connected == null ? 'unknown' : s.ibkr.connected ? 'Logged in' : 'Cut off (1100)',
      sub: s.ibkr.connected === false ? [s.ibkr.down_since != null && outage(s.ibkr.down_since), 'no orders or prices; restart ib-gateway if it lasts'].filter(Boolean).join(' · ') : undefined,
      tone: s.ibkr.connected ? 'ok' : s.ibkr.connected == null ? 'off' : 'bad' },
    { k: 'Dashboard ↔ Gateway', v: s.broker.connected ? 'Connected (read-only)' : 'Not connected', sub: s.broker.connected ? `checked ${ago((s.broker.checked_at ?? 0) * 1000)}` : s.broker.error ?? undefined, tone: s.broker.connected ? 'ok' : 'warn' },
    { k: 'Live feed from the bot', v: s.live.connected ? 'Connected' : 'Not connected', sub: 'prices and intraday z, display only', tone: s.live.connected ? 'ok' : 'warn' },
    { k: 'Last close decided', v: s.last_close_t ? `${fmtWhen(s.last_close_t)} ET` : 'none journaled', sub: 'after the 18:00 ET history fetch', tone: s.last_close_t && Date.now() - s.last_close_t < 4 * 86400_000 ? 'ok' : 'warn' },
    { k: 'Baskets', v: `${s.baskets_loaded ?? '?'} running / ${s.baskets_configured} configured`, tone: s.baskets_loaded === s.baskets_configured ? 'ok' : 'warn' },
    { k: 'Market data lines', v: `${s.symbols} / ${s.market_data_lines}`, sub: 'one per symbol; delayed quotes, UI only', tone: s.symbols > s.market_data_lines * 0.9 ? 'warn' : 'ok' },
    { k: 'State saved', v: s.state_saved_at ? ago(s.state_saved_at * 1000) : '—', sub: 'newest file in state/', tone: 'ok' },
  ]
}

function SymbolCaps({ caps, cap }: { caps: SymbolCap[] | null; cap: number | null }) {
  const [all, setAll] = useState(false)
  if (!caps) return <p className="m-0 p-4 text-sm text-muted">This bot version doesn't report per-symbol usage.</p>
  if (!caps.length) return <p className="m-0 p-4 text-sm text-muted">Nothing held or reserved.</p>
  const shown = all ? caps : caps.slice(0, 8)
  return (
    <>
      <ul className="m-0 flex list-none flex-col gap-2.5 px-4 py-3.5">
        {shown.map(c => {
          const pct = c.pct ?? 0
          const tone = pct >= 0.9 ? 'bg-alert' : pct >= 0.7 ? 'bg-warn' : 'bg-accent'
          return (
            <li key={c.symbol} className="flex flex-col gap-1">
              <div className="flex items-baseline gap-2 text-sm">
                <span className="num w-14 font-medium">{c.symbol}</span>
                <span className="num">{c.pct != null ? `${Math.round(c.pct * 100)}%` : '—'}</span>
                <span className="num text-[13px] text-muted">{usd(c.usd)}</span>
                <span className="ml-auto text-xs text-muted">{c.baskets} basket{c.baskets === 1 ? '' : 's'} trade it</span>
              </div>
              <div className="h-1.5 overflow-hidden rounded-[3px] bg-line" role="meter" aria-label={`${c.symbol} cap used`}
                aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(pct * 100)}>
                <div className={`h-full ${tone}`} style={{ width: `${Math.min(100, pct * 100)}%` }} />
              </div>
            </li>
          )
        })}
      </ul>
      <div className="flex flex-wrap items-center gap-3 border-t border-line px-4 py-2.5 text-xs text-muted">
        <span>|net| per symbol across baskets, incl. working entries, vs the {usd(cap)} cap. A full symbol blocks every basket that trades it.</span>
        {caps.length > 8 && (
          <button type="button" onClick={() => setAll(!all)} className="ml-auto min-h-9 cursor-pointer rounded-md border border-line bg-transparent px-3 text-[13px] text-link">
            {all ? 'Fewer' : `All ${caps.length}`}
          </button>
        )}
      </div>
    </>
  )
}

const runTitle = (r: LogRun) => (r.started_at == null ? 'Before the first journaled start' : `${r.current ? 'Current run' : 'Run'}, started ${fmtWhen(r.started_at)} ET`)

function LogLines({ run }: { run: LogRun }) {
  if (!run.lines.length) return <div className="text-muted">None.</div>
  return (
    <>
      {run.lines.map((l, i) => (
        <div key={i} className="whitespace-nowrap">
          <span className="text-muted">{l.last.slice(5)}</span>{' '}
          <span className={l.level === 'error' ? 'text-bad' : 'text-short'}>{l.level}</span>{' '}
          {l.count > 1 && <span className="rounded bg-raised px-1 text-muted" title={`first ${l.time}`}>×{l.count}</span>}{' '}{l.message}
        </div>
      ))}
    </>
  )
}

function Logs({ runs }: { runs: LogRun[] }) {
  if (!runs.length) return <div className="num px-4 py-3.5 text-xs text-muted">No warnings or errors in the recent log.</div>
  const current = runs.find(r => r.current)
  const older = runs.filter(r => !r.current)
  return (
    <div className="num max-h-[480px] overflow-auto px-4 pt-2.5 pb-3.5 text-xs leading-[1.8]">
      {current ? (
        <>
          <div className="mb-1 font-sans text-[13px] font-semibold text-text">{runTitle(current)}</div>
          {current.lines.length ? <LogLines run={current} /> : <div className="text-muted">No warnings or errors since the bot started.</div>}
        </>
      ) : <div className="text-muted">Nothing logged by the current run.</div>}
      {older.map(r => (
        <details key={r.started_at ?? 'before'} className="mt-3 border-t border-line2 pt-2">
          <summary className="cursor-pointer font-sans text-[13px] text-dim">{runTitle(r)} · {r.lines.length} distinct</summary>
          <div className="mt-1"><LogLines run={r} /></div>
        </details>
      ))}
    </div>
  )
}

export default function System() {
  const { data: s, error, isLoading } = useSystem()
  const setFlag = useSetFlag()
  const clearHalt = useClearHalt()
  const [ask, setAsk] = useState<Ask>(null)
  const [removeBooks, setRemoveBooks] = useState(true)
  const [done, setDone] = useState<string | null>(null)
  if (isLoading) return <Loading />
  if (error || !s) return <Failed error={error} />

  const close = () => { setAsk(null); setFlag.reset(); clearHalt.reset() }
  const lim = s.limits
  const eq = s.equity_at_start_usd

  return (
    <div className="grid grid-cols-[minmax(0,1fr)] items-start gap-4 lg:grid-cols-2">
      <div className="flex flex-col gap-4">
        <Card>
          <CardHead title="Controls" />
          <div className="flex flex-col gap-4 p-4">
            <Control name="Kill switch" on={s.flags.kill} tone="warn" button={s.flags.kill ? 'Turn kill switch off' : 'Turn kill switch on'}
              onClick={() => setAsk({ kind: 'kill', on: !s.flags.kill })}>
              No new entries. Open baskets keep running and still exit normally. Writes <span className="num">state/KILL</span>.
            </Control>
            <div className="h-px bg-line" />
            <Control name="Flatten everything" on={s.flags.flatten} tone="bad" button={s.flags.flatten ? 'Stop flattening' : 'Flatten…'}
              onClick={() => setAsk({ kind: 'flatten', on: !s.flags.flatten })}>
              Every open basket exits at the next session with MOC orders (fills at that close). Writes <span className="num">state/FLATTEN</span>.
            </Control>
            <p className="m-0 text-xs text-muted">Each action asks you to confirm, and is written to the dashboard's audit log. The bot reads these files at each decision.</p>
          </div>
        </Card>

        <Card tone={s.halts.length ? 'bad' : undefined}>
          <CardHead title={<>Halted baskets <span className={`num ml-1 text-[13px] ${s.halts.length ? 'text-bad' : 'text-muted'}`}>{s.halts.length}</span></>} />
          {done && <p className="m-0 border-b border-line px-4 py-3 text-sm text-good">{done}</p>}
          {!s.halts.length && <p className="m-0 p-4 text-sm text-muted">None. Every basket can trade.</p>}
          {s.halts.map(h => (
            <div key={h.id} className="flex flex-col gap-3.5 border-b border-line2 p-4 last:border-b-0">
              <div className="flex flex-wrap items-baseline gap-x-4 gap-y-1">
                <Link to={`/baskets/${h.id}`} className="num text-[15px] font-semibold">{h.id}</Link>
                {h.halted_at && <span className="text-[13px] text-muted">halted {fmtWhen(h.halted_at)} ET</span>}
              </div>
              <div className="num rounded-md border border-line bg-bg px-3 py-2.5 text-[13px] text-bad">{h.reason ?? 'no reason recorded'}</div>
              <ol className="m-0 list-decimal pl-5 text-sm leading-7 text-dim">
                <li>Fix the position at the broker if needed (<Link to="/orders">compare on Orders</Link>).</li>
                <li>Stop the bot, outside 09:00–16:15 ET: <span className="num text-text">docker compose stop funtrade</span></li>
                <li>Clear the halt: removes the basket's state file{Object.keys(h.book_keys).length > 0 && ' and, if chosen, its book entries'} (backed up first).</li>
                <li>Start the bot: <span className="num text-text">docker compose start funtrade</span>. The basket starts FLAT.</li>
              </ol>
              {Object.keys(h.book_keys).length > 0 && (
                <label className="flex items-start gap-2.5 text-sm text-dim">
                  <input type="checkbox" checked={removeBooks} onChange={e => setRemoveBooks(e.target.checked)} className="mt-1 size-4" />
                  <span>Also remove its book entries: <span className="num">{Object.entries(h.book_keys).map(([k, q]) => `${k} ${qty(q)}`).join(', ')}</span>. Do this when the broker doesn't hold them (or you closed them by hand).</span>
                </label>
              )}
              <div className="flex flex-wrap items-center gap-2.5">
                <button type="button" disabled={!s.can_edit_state} onClick={() => setAsk({ kind: 'halt', halt: h })}
                  className="min-h-11 rounded-lg border border-line bg-raised px-[18px] text-sm font-semibold text-text enabled:cursor-pointer enabled:border-bad-bd enabled:bg-bad-bg enabled:text-bad disabled:cursor-not-allowed disabled:text-muted">
                  Clear halt
                </button>
                {!s.can_edit_state && <span className="text-[13px] text-muted">Available once the bot is stopped</span>}
              </div>
            </div>
          ))}
        </Card>
      </div>

      <div className="flex flex-col gap-4">
        <Card>
          <CardHead title="Health" />
          <dl className="m-0 flex flex-col px-4 pt-2 pb-3">
            {healthRows(s).map(h => (
              <div key={h.k} className="flex flex-wrap items-baseline gap-x-3 gap-y-0.5 border-b border-line2 py-2.5 last:border-b-0">
                <dt className="flex w-[200px] shrink-0 items-center gap-2 text-sm text-dim"><Dot tone={h.tone} />{h.k}</dt>
                <dd className="m-0 min-w-[180px] flex-1 text-sm">{h.v} {h.sub && <span className="text-[13px] text-muted">{h.sub}</span>}</dd>
              </div>
            ))}
          </dl>
        </Card>

        <Card>
          <CardHead title={<>Risk limits <span className="ml-1 text-[13px] font-normal text-muted">{eq ? `from config/risk.json at startup equity ${usd(eq)}` : 'from config/risk.json'}</span></>} />
          <div className="grid grid-cols-2 gap-3.5 px-4 py-3.5">
            {[
              ['Per basket', eq && s.risk_config.allocation_per_basket ? usd(eq * Number(s.risk_config.allocation_per_basket)) : '—', `${(Number(s.risk_config.allocation_per_basket ?? 0) * 100).toFixed(1)}%`],
              ['Gross cap', usd(lim?.max_gross_exposure_usd), `${(Number(s.risk_config.max_gross_exposure ?? 0) * 100).toFixed(0)}%`],
              ['Per-symbol net cap', usd(lim?.max_symbol_net_usd), `${(Number(s.risk_config.max_symbol_net ?? 0) * 100).toFixed(0)}%`],
              ['Basket loss limit', usd(lim?.max_basket_loss_usd), `${(Number(s.risk_config.max_basket_loss ?? 0) * 100).toFixed(0)}% of allocation`],
            ].map(([k, v, sub]) => (
              <div key={k}><div className="text-xs text-muted">{k}</div><div className="num mt-0.5 text-base">{v} <span className="text-[13px] text-muted">{sub}</span></div></div>
            ))}
          </div>
        </Card>

        <Card>
          <CardHead title="Per-symbol cap usage" />
          <SymbolCaps caps={s.symbol_caps} cap={lim?.max_symbol_net_usd ?? null} />
        </Card>

        <Card>
          <CardHead title="Warnings and errors" />
          <Logs runs={s.logs} />
          <p className="m-0 border-t border-line px-4 py-2.5 text-xs text-muted">From logs/funtrade.log, by bot run (newest first), repeats folded (×n), times in New York. Earlier runs are collapsed: a restart may have fixed them.</p>
        </Card>
      </div>

      {ask?.kind === 'kill' && (
        <Confirm title={ask.on ? 'Turn kill switch on?' : 'Turn kill switch off?'} word="KILL" action={ask.on ? 'Turn on' : 'Turn off'} tone="warn"
          busy={setFlag.isPending} error={setFlag.error?.message} onClose={close}
          onConfirm={() => setFlag.mutate({ flag: 'kill', on: ask.on, confirm: 'KILL' }, { onSuccess: close })}>
          {ask.on
            ? <ul className="m-0 list-disc pl-[18px] leading-7"><li>No basket opens a new position from the next decision.</li><li>Open baskets keep running and exit normally.</li><li>MOC entries already at the broker are not cancelled.</li></ul>
            : 'Baskets may enter again from the next close.'}
        </Confirm>
      )}
      {ask?.kind === 'flatten' && (
        <Confirm title={ask.on ? 'Flatten all baskets?' : 'Stop flattening?'} word="FLATTEN" action={ask.on ? 'Flatten' : 'Stop'} tone={ask.on ? 'bad' : 'warn'}
          busy={setFlag.isPending} error={setFlag.error?.message} onClose={close}
          onConfirm={() => setFlag.mutate({ flag: 'flatten', on: ask.on, confirm: 'FLATTEN' }, { onSuccess: close })}>
          {ask.on
            ? <ul className="m-0 list-disc pl-[18px] leading-7"><li>Exit orders go out at the next session start and fill at that day's close.</li><li>Entries working now still fill, then exit.</li><li>Stays on until you turn it off.</li></ul>
            : 'Baskets stop exiting because of FLATTEN. Those already exiting finish their exit.'}
        </Confirm>
      )}
      {ask?.kind === 'halt' && (
        <Confirm title={`Clear the halt on ${ask.halt.id}?`} word={ask.halt.id} action="Clear halt" tone="bad"
          busy={clearHalt.isPending} error={clearHalt.error?.message} onClose={close}
          onConfirm={() => clearHalt.mutate({ basket: ask.halt.id, confirm: ask.halt.id, remove_books: removeBooks }, {
            onSuccess: r => { close(); setDone(`${ask.halt.id} cleared${r.removed_keys.length ? `, removed ${r.removed_keys.join(', ')}` : ''}. Backup: ${r.backup}. Start the bot to resume.`) },
          })}>
          Deletes <span className="num">state/baskets/{ask.halt.id}.json</span>{removeBooks && Object.keys(ask.halt.book_keys).length > 0 && <> and its book entries ({Object.keys(ask.halt.book_keys).join(', ')})</>}.
          A backup is kept. On the next start the basket is FLAT and reconciles again.
        </Confirm>
      )}
    </div>
  )
}
