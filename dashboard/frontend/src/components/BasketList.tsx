import { useMemo, useState } from 'react'
import { Link } from 'react-router-dom'
import { useStream } from '../lib/live'
import { pnlClass, usd, z as fmtZ } from '../lib/format'
import type { BasketRow } from '../lib/types'
import { Pills, StateChip, Td, Th, sideClass } from './ui'
import { ZGauge, ZLegend } from './ZGauge'

type Filter = 'all' | 'active' | 'pending' | 'halted' | 'near'

const PENDING = ['ENTRY_PENDING', 'EXIT_PENDING', 'RECOVERY_PENDING']
const near = (b: BasketRow) => b.state === 'FLAT' && b.close_z != null && Math.abs(b.close_z) >= 0.75 * b.entry_z

const FILTERS: Record<Filter, (b: BasketRow) => boolean> = {
  all: () => true,
  active: b => b.state !== 'FLAT' || b.in_books,
  pending: b => PENDING.includes(b.state),
  halted: b => b.state === 'SYSTEM_HALTED',
  near,
}

/** Most interesting first: halted, working, in position, near entry, then by |z|. */
function rank(b: BasketRow) {
  if (b.state === 'SYSTEM_HALTED') return 0
  if (PENDING.includes(b.state)) return 1
  if (b.state === 'IN_POSITION') return 2
  if (near(b)) return 3
  return 4
}

function position(b: BasketRow) {
  if (b.state === 'ENTRY_PENDING') return `FLAT → ${b.position}`
  if (b.state === 'EXIT_PENDING') return `${b.position} → FLAT`
  return b.position
}

/** What a human wants to know about this basket, in a few words. */
export function note(b: BasketRow): string {
  if (b.state === 'SYSTEM_HALTED') return b.halt_reason ?? 'Halted: needs a human'
  if (b.skipped_today) return `Entry skipped: ${b.skipped_today}`
  if (b.state === 'ENTRY_PENDING') return `MOC entry working · ${b.working_legs} legs`
  if (b.state === 'EXIT_PENDING') return `MOC exit working · ${b.working_legs} legs`
  if (b.state === 'RECOVERY_PENDING') return 'Unbalanced: recovery sweep at next session'
  if (b.pending_action !== 'NONE') return `${b.pending_action.replace('_', ' ').toLowerCase()} at next session`
  if (b.state === 'IN_POSITION') {
    return b.position === 'LONG' ? `Exits when z rises above ${fmtZ(-b.exit_z)}` : `Exits when z falls below ${fmtZ(b.exit_z)}`
  }
  if (near(b)) return `Near entry (±${b.entry_z.toFixed(2)})`
  return ''
}

const pnlOf = (b: BasketRow) => (b.in_books ? (b.unrealized == null ? null : b.realized + b.unrealized) : b.realized || null)

export function BasketList({ rows, initial = 'all', limit }: { rows: BasketRow[]; initial?: Filter; limit?: number }) {
  const [filter, setFilter] = useState<Filter>(initial)
  const [q, setQ] = useState('')
  const { live } = useStream()

  const counts = useMemo(() => Object.fromEntries((Object.keys(FILTERS) as Filter[]).map(f => [f, rows.filter(FILTERS[f]).length])), [rows])
  const query = q.trim().toUpperCase()
  const shown = rows
    .filter(FILTERS[filter])
    .filter(b => !query || b.id.toUpperCase().includes(query) || b.tickers.some(t => t.includes(query)))
    .sort((a, b) => rank(a) - rank(b) || Math.abs(b.close_z ?? 0) - Math.abs(a.close_z ?? 0))
  const visible = limit ? shown.slice(0, limit) : shown
  const z = (b: BasketRow) => b.close_z

  return (
    <>
      <div className="flex flex-wrap items-center gap-3 border-b border-line px-4 py-3.5">
        <h2 className="m-0 text-base font-semibold">Baskets</h2>
        <Pills label="Filter" value={filter} onChange={setFilter} options={[
          { id: 'all', label: 'All', count: counts.all }, { id: 'active', label: 'Active', count: counts.active },
          { id: 'pending', label: 'Pending', count: counts.pending }, { id: 'halted', label: 'Halted', count: counts.halted },
          { id: 'near', label: 'Near entry', count: counts.near },
        ]} />
        <label className="flex w-full items-center gap-2 text-[13px] text-muted sm:ml-auto sm:w-auto">
          Search
          <input type="search" value={q} onChange={e => setQ(e.target.value)} placeholder="symbol or basket"
            className="min-h-9 w-full rounded-md border border-line bg-bg px-2.5 text-text sm:w-[180px]" />
        </label>
      </div>

      {/* desktop: table */}
      <div className="hidden overflow-x-auto md:block">
        <table className="w-full border-collapse">
          <thead><tr>
            <Th>Basket</Th><Th>State</Th><Th>z (last close)</Th><Th>Position</Th><Th>Held</Th><Th right>P&amp;L</Th><Th>Note</Th>
          </tr></thead>
          <tbody>
            {visible.map(b => (
              <tr key={b.id} className="hover:bg-[#1A2028]">
                <Td><Link to={`/baskets/${b.id}`} className="num font-medium">{b.id}</Link><div className="mt-0.5 text-xs text-muted">{b.tickers.join(' · ')}</div></Td>
                <Td><StateChip state={b.state} /></Td>
                <Td>
                  <div className="flex items-center gap-2.5">
                    <ZGauge z={z(b)} entry={b.entry_z} exit={b.exit_z} />
                    <span className="num text-xs text-muted">±{b.entry_z.toFixed(2)} / {fmtZ(b.exit_z)}</span>
                  </div>
                  {live?.baskets[b.id]?.z != null && <div className="num mt-0.5 text-[11px] text-muted">intraday {live.baskets[b.id].z!.toFixed(2)}</div>}
                </Td>
                <Td className={sideClass(b.position)}>{position(b)}</Td>
                <Td className="num text-dim">{b.held_days != null ? `${b.held_days} d` : '—'}</Td>
                <Td right className={`num ${pnlClass(pnlOf(b))}`}>{usd(pnlOf(b), { sign: true })}</Td>
                <Td className="min-w-[220px] text-[13px] whitespace-normal text-dim">{note(b)}</Td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* phone: cards */}
      <ul className="m-0 flex list-none flex-col gap-2 p-3 md:hidden">
        {visible.map(b => (
          <li key={b.id}>
            <Link to={`/baskets/${b.id}`} className="flex flex-col gap-2 rounded-[10px] border border-line bg-bg px-3.5 py-3 text-text">
              <div className="flex items-center gap-2">
                <span className="num text-sm font-medium">{b.id}</span>
                <StateChip state={b.state} small />
                <span className={`num ml-auto text-sm ${pnlClass(pnlOf(b))}`}>{usd(pnlOf(b), { sign: true })}</span>
              </div>
              <div className="flex items-center gap-2.5">
                <div className="flex-1"><ZGauge z={z(b)} entry={b.entry_z} exit={b.exit_z} compact /></div>
                <span className={`w-11 text-right text-xs ${sideClass(b.position)}`}>{b.position}</span>
              </div>
              {note(b) && <span className="text-xs text-muted">{note(b)}</span>}
            </Link>
          </li>
        ))}
      </ul>

      <div className="flex flex-wrap gap-4 px-4 py-3 text-[13px] text-muted">
        {limit && shown.length > limit ? <Link to="/baskets">+ {shown.length - limit} more →</Link> : <span>{shown.length} shown</span>}
        {!visible.length && <span>Nothing matches.</span>}
        <span className="hidden md:ml-auto md:block"><ZLegend /></span>
      </div>
    </>
  )
}
