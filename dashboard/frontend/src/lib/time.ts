import { useEffect, useState } from 'react'

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
const hhmm = (min: number) => `${pad(Math.floor(((min % 1440) + 1440) % 1440 / 60))}:${pad(((min % 60) + 60) % 60)}`

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

function inMin(min: number) {
  if (min < 60) return `${min}m`
  return `${Math.floor(min / 60)}h ${pad(min % 60)}m`
}

const WEEKDAYS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri']

/** Where the US regular session is now. Exchange holidays and half days are not known here. */
export function session(now = Date.now()) {
  const ny = parts(now, NY)
  const il = parts(now, IL)
  const nyMin = ny.hour * 60 + ny.minute
  const offset = (il.hour * 60 + il.minute - nyMin + 1440 * 2) % 1440   // IL − NY, usually 7h
  const both = (nyAt: number) => ({ ny: hhmm(nyAt), il: hhmm(nyAt + offset) })
  const weekday = WEEKDAYS.includes(ny.weekday)
  const open = weekday && nyMin >= 570 && nyMin < 960
  return {
    open,
    nyClock: `${pad(ny.hour)}:${pad(ny.minute)}`,
    ilClock: `${pad(il.hour)}:${pad(il.minute)}`,
    cutoff: { ...both(950), in: open && nyMin < 950 ? inMin(950 - nyMin) : null },
    close: { ...both(960), in: open ? inMin(960 - nyMin) : null },
    eod: both(1080),
    opens: both(570),
    weekday,
  }
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
