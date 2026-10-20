import type { ReactNode } from 'react'
import { Link } from 'react-router-dom'
import { AlertTriangle, ChevronRight, Clock } from 'lucide-react'
import type { MasterState, Side } from '../lib/types'

export function Card({ children, className = '', tone }: { children: ReactNode; className?: string; tone?: 'bad' }) {
  return <section className={`rounded-[10px] border bg-panel ${tone === 'bad' ? 'border-bad-bd' : 'border-line'} ${className}`}>{children}</section>
}

export function CardHead({ title, children }: { title: ReactNode; children?: ReactNode }) {
  return (
    <div className="flex flex-wrap items-center gap-3 border-b border-line px-4 py-3.5">
      <h2 className="m-0 text-base font-semibold">{title}</h2>
      {children}
    </div>
  )
}

export function Tile({ label, children, sub, className = '' }: { label: string; children: ReactNode; sub?: ReactNode; className?: string }) {
  return (
    <div className={`flex flex-col gap-1.5 rounded-[10px] border border-line bg-panel px-3.5 py-3 md:px-[18px] md:py-4 ${className}`}>
      <span className="text-xs text-muted md:text-[13px]">{label}</span>
      <span className="num text-xl font-semibold md:text-[26px]">{children}</span>
      {sub && <span className="text-[11px] text-muted md:text-xs">{sub}</span>}
    </div>
  )
}

export function Dot({ tone }: { tone: 'ok' | 'warn' | 'bad' | 'off' }) {
  const c = { ok: 'bg-ok', warn: 'bg-warn', bad: 'bg-alert', off: 'bg-edge' }[tone]
  return <span aria-hidden className={`inline-block size-2 shrink-0 rounded-full ${c}`} />
}

const STATE_STYLE: Record<MasterState, string> = {
  FLAT: 'bg-raised text-[#AEB7C3] border-line',
  ENTRY_PENDING: 'bg-long-bg text-long border-long-bd',
  EXIT_PENDING: 'bg-long-bg text-long border-long-bd',
  RECOVERY_PENDING: 'bg-short-bg text-short border-short-bd',
  IN_POSITION: 'bg-good-bg text-good border-good-bd',
  SYSTEM_HALTED: 'bg-bad-bg text-bad border-bad-bd',
}

export function StateChip({ state, small }: { state: MasterState; small?: boolean }) {
  return (
    <span className={`inline-block rounded border font-semibold tracking-wide whitespace-nowrap ${small ? 'px-1.5 py-0.5 text-[10px]' : 'px-2 py-[3px] text-xs'} ${STATE_STYLE[state] ?? STATE_STYLE.FLAT}`}>
      {state}
    </span>
  )
}

export const sideClass = (s: Side | string) => (s === 'LONG' || s === 'BUY' ? 'text-long font-semibold' : s === 'SHORT' || s === 'SELL' ? 'text-short font-semibold' : 'text-muted')

export function Banner({ tone, children, to, action }: { tone: 'bad' | 'info'; children: ReactNode; to?: string; action?: string }) {
  const Icon = tone === 'bad' ? AlertTriangle : Clock
  const body = (
    <>
      <Icon size={18} aria-hidden className={tone === 'bad' ? 'shrink-0 text-alert' : 'shrink-0 text-accent'} />
      <span className="flex-1 text-sm text-text">{children}</span>
      {to && <span className="hidden text-sm text-link md:inline">{action ?? 'Open'} →</span>}
      {to && <ChevronRight size={18} aria-hidden className="text-muted md:hidden" />}
    </>
  )
  const cls = `flex min-h-11 items-center gap-3 rounded-[10px] border px-4 py-3 ${tone === 'bad' ? 'border-bad-bd bg-[#2A1517]' : 'border-line bg-panel'}`
  return to ? <Link to={to} className={cls}>{body}</Link> : <div className={cls}>{body}</div>
}

export function Pills<T extends string>({ value, options, onChange, label }: {
  value: T; options: { id: T; label: string; count?: number }[]; onChange: (v: T) => void; label: string
}) {
  return (
    <div role="group" aria-label={label} className="flex flex-wrap gap-1.5">
      {options.map(o => (
        <button key={o.id} type="button" aria-pressed={value === o.id} onClick={() => onChange(o.id)}
          className={`min-h-8 cursor-pointer rounded-md border px-3 py-1.5 text-[13px] ${value === o.id ? 'border-edge bg-sel text-text' : 'border-line bg-transparent text-dim hover:bg-raised'}`}>
          {o.label}{o.count != null && <span className="num ml-1.5 text-muted">{o.count}</span>}
        </button>
      ))}
    </div>
  )
}

export function Loading({ what = 'Loading' }: { what?: string }) {
  return <div className="p-8 text-sm text-muted">{what}…</div>
}

export function Failed({ error }: { error: unknown }) {
  return <div className="m-4 rounded-[10px] border border-bad-bd bg-bad-bg p-4 text-sm text-bad">Couldn't load: {String((error as Error)?.message ?? error)}</div>
}

export function Th({ children, right, className = '' }: { children?: ReactNode; right?: boolean; className?: string }) {
  return <th className={`border-b border-line px-3 py-2.5 text-xs font-medium whitespace-nowrap text-muted ${right ? 'text-right' : 'text-left'} ${className}`}>{children}</th>
}

export function Td({ children, right, className = '' }: { children?: ReactNode; right?: boolean; className?: string }) {
  return <td className={`border-b border-line2 px-3 py-3 text-sm whitespace-nowrap ${right ? 'text-right' : ''} ${className}`}>{children}</td>
}
