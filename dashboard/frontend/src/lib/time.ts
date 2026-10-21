import { useEffect, useState } from 'react'
import type { PriceSource, PriceSummary, SessionInfo } from './types'

// The bot lives on New York time; the owner on Israel time. Show both, the phone IL first.
const NY = 'America/New_York'
const IL = 'Asia/Jerusalem'

function parts(ms: number, tz: string) {
  const f = new Intl.DateTimeFormat('en-GB', {
    timeZone: tz, weekday: 'short', year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  })
  const p = Object.fromEntries(f.formatToParts(ms).map(x => [x.type, x.value]))
  const hour = Number(p.hour) % 24
  return { weekday: p.weekday as string, date: `${p.year}-${p.month}-${p.day}`, hour, minute: Number(p.minute), second: Number(p.second) }
}

const pad = (n: number) => String(n).padStart(2, '0')

export const fmtTime = (ms: number, tz: 'NY' | 'IL') => {
  const p = parts(ms, tz === 'NY' ? NY : IL)
  return `${pad(p.hour)}:${pad(p.minute)}`
}

export function fmtWhen(ms: number, tz: 'NY' | 'IL' = 'NY') {
  const p = parts(ms, tz === 'NY' ? NY : IL)
  const today = parts(Date.now(), tz === 'NY' ? NY : IL).date
  const t = `${pad(p.hour)}:${pad(p.minute)}`
  if (p.date === today) return t
  const d = new Date(p.date + 'T00:00:00Z').toLocaleDateString('en-US', { month: 'short', day: 'numeric', timeZone: 'UTC' })
  return `${d} ${t}`
}

export const fmtDay = (iso: string) =>
  new Date(iso + 'T00:00:00Z').toLocaleDateString('en-US', { month: 'short', day: 'numeric', timeZone: 'UTC' })

export function ago(ms: number | null | undefined): string {
  if (!ms) return 'never'
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000))
  if (s < 60) return `${s}s ago`
  if (s < 3600) return `${Math.round(s / 60)}m ago`
  if (s < 86400) return `${Math.round(s / 3600)}h ago`
  return `${Math.round(s / 86400)}d ago`
}

export function inMin(min: number) {
  if (min < 60) return `${min}m`
  return `${Math.floor(min / 60)}h ${pad(min % 60)}m`
}

const nyDate = (ms: number) => parts(ms, NY).date
const both = (ms: number) => ({ ny: fmtTime(ms, 'NY'), il: fmtTime(ms, 'IL') })

/**
 * The session line from /api/session (the backend's NYSE calendar: holidays, 13:00 early
 * closes) at `now`: what's next, in both zones, with countdowns. `opensOn` names the day the
 * next session opens when it isn't today ("Mon", or "Nov 27" a week or more out).
 */
export function sessionView(info: SessionInfo, now = Date.now()) {
  const open = now >= info.open_ms && now < info.close_ms   // not info.open: it was true when fetched
  const today = nyDate(now) === info.day
  const until = (ms: number) => (now < ms ? inMin(Math.ceil((ms - now) / 60_000)) : null)
  const days = Math.round((Date.parse(info.day + 'T12:00:00Z') - Date.parse(nyDate(now) + 'T12:00:00Z')) / 86_400_000)
  const opensOn = today ? null
    : days < 7 ? new Date(info.day + 'T12:00:00Z').toLocaleDateString('en-US', { weekday: 'short', timeZone: 'UTC' })
    : fmtDay(info.day)
  return {
    open,
    nyClock: fmtTime(now, 'NY'),
    ilClock: fmtTime(now, 'IL'),
    cutoff: { ...both(info.cutoff_ms), in: open ? until(info.cutoff_ms) : null },
    close: { ...both(info.close_ms), in: open ? until(info.close_ms) : null },
    opens: { ...both(info.open_ms), in: !open ? until(info.open_ms) : null },
    opensOn,
    eod: both(info.eod_ms),
    holiday: info.holiday,
    earlyClose: info.early_close,
    known: info.known,
  }
}

/** Where a leg's price comes from: the day's close (after the bot's EOD), or the bot's feed,
 * which is IBKR delayed data (~15 min behind the market in session). */
export function priceNote(p: { source: PriceSource; price_day: string | null; price_t: number | null }) {
  if (p.source === 'close') return p.price_day ? `close ${fmtDay(p.price_day)}` : 'last close'
  if (p.source === 'delayed') return p.price_t ? `delayed · ${fmtWhen(p.price_t)} ET` : 'delayed'
  return 'no price'
}

/** The same for the account's open P&L (every held leg). '' when nothing is held. */
export function pricesNote(s: PriceSummary) {
  const base = s.source === 'close' ? (s.close_day ? `at the ${fmtDay(s.close_day)} close` : 'at the last close')
    : s.source === 'delayed' ? `delayed quotes${s.oldest_t ? `, oldest ${fmtWhen(s.oldest_t)} ET` : ''}`
    : s.source === 'mixed' ? 'closes and delayed quotes' : ''
  const missing = s.missing ? `${s.missing} leg${s.missing > 1 ? 's' : ''} without a price` : ''
  return [base, missing].filter(Boolean).join(' · ')
}

/** Re-renders every `ms` (clocks, countdowns, "x ago"). */
export function useNow(ms = 30_000) {
  const [now, setNow] = useState(Date.now())
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), ms)
    return () => clearInterval(id)
  }, [ms])
  return now
}
