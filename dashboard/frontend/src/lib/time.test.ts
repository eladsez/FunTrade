import { afterEach, describe, expect, it, vi } from 'vitest'
import { ago, fmtDay, fmtTime, fmtWhen, inMin, outage, priceNote, pricesNote, sessionView } from './time'
import type { SessionInfo } from './types'

// 2026-10-21 (Wed): EDT = UTC−4, Israel = UTC+3 (IDT until Oct 25).
const ny = (iso: string) => Date.parse(`${iso}-04:00`)
const wednesday: SessionInfo = {
  open: true, today_is_session: true, holiday: null, early_close: false, day: '2026-10-21',
  open_ms: ny('2026-10-21T09:30'), close_ms: ny('2026-10-21T16:00'), cutoff_ms: ny('2026-10-21T15:50'),
  eod_ms: ny('2026-10-21T18:00'), known: true,
}

afterEach(() => vi.useRealTimers())

describe('sessionView', () => {
  it('in session: countdowns to the MOC cutoff and the close, both zones', () => {
    const s = sessionView(wednesday, ny('2026-10-21T14:05'))
    expect(s.open).toBe(true)
    expect(s.nyClock).toBe('14:05')
    expect(s.ilClock).toBe('21:05')
    expect(s.cutoff).toEqual({ ny: '15:50', il: '22:50', in: '1h 45m' })
    expect(s.close).toEqual({ ny: '16:00', il: '23:00', in: '1h 55m' })
    expect(s.opensOn).toBeNull()
  })
  it('past the cutoff only the close counts down', () => {
    const s = sessionView(wednesday, ny('2026-10-21T15:55'))
    expect(s.cutoff.in).toBeNull()
    expect(s.close.in).toBe('5m')
  })
  it('before the open: closed, opening today', () => {
    const s = sessionView(wednesday, ny('2026-10-21T08:00'))
    expect(s.open).toBe(false)
    expect(s.opens).toEqual({ ny: '09:30', il: '16:30', in: '1h 30m' })
    expect(s.opensOn).toBeNull()
  })
  it('stale data from before the open still flips to open on time', () => {
    const s = sessionView({ ...wednesday, open: false }, ny('2026-10-21T09:31'))
    expect(s.open).toBe(true)
  })
  it('after a Friday close the next session is named', () => {
    const monday = { ...wednesday, day: '2026-10-26', open_ms: ny('2026-10-26T09:30') }
    expect(sessionView(monday, ny('2026-10-23T17:00')).opensOn).toBe('Mon')
  })
  it('a week or more out shows the date', () => {
    expect(sessionView({ ...wednesday, day: '2026-11-02', open_ms: ny('2026-11-02T09:30') }, ny('2026-10-23T17:00')).opensOn).toBe('Nov 2')
  })
  it('passes holidays, early closes and calendar gaps through', () => {
    const s = sessionView({ ...wednesday, holiday: 'Thanksgiving Day', early_close: true, known: false }, ny('2026-10-21T08:00'))
    expect([s.holiday, s.earlyClose, s.known]).toEqual(['Thanksgiving Day', true, false])
  })
})

describe('prices', () => {
  it('names where a leg price comes from', () => {
    expect(priceNote({ source: 'close', price_day: '2026-10-20', price_t: null })).toBe('close Oct 20')
    expect(priceNote({ source: 'close', price_day: null, price_t: null })).toBe('last close')
    expect(priceNote({ source: 'delayed', price_day: null, price_t: null })).toBe('delayed')
    expect(priceNote({ source: null, price_day: null, price_t: null })).toBe('no price')
  })
  it('delayed prices show their time', () => {
    vi.useFakeTimers({ toFake: ['Date'] })
    vi.setSystemTime(ny('2026-10-21T15:00'))
    expect(priceNote({ source: 'delayed', price_day: null, price_t: ny('2026-10-21T14:32') })).toBe('delayed · 14:32 ET')
  })
  it('summarizes the account', () => {
    vi.useFakeTimers({ toFake: ['Date'] })
    vi.setSystemTime(ny('2026-10-21T15:00'))
    expect(pricesNote({ source: 'close', oldest_t: null, close_day: '2026-10-20', missing: 0 })).toBe('at the Oct 20 close')
    expect(pricesNote({ source: 'close', oldest_t: null, close_day: null, missing: 0 })).toBe('at the last close')
    expect(pricesNote({ source: 'delayed', oldest_t: ny('2026-10-21T14:40'), close_day: null, missing: 1 }))
      .toBe('delayed quotes, oldest 14:40 ET · 1 leg without a price')
    expect(pricesNote({ source: 'mixed', oldest_t: null, close_day: null, missing: 2 })).toBe('closes and delayed quotes · 2 legs without a price')
    expect(pricesNote({ source: null, oldest_t: null, close_day: null, missing: 0 })).toBe('')
  })
})

describe('time formatting', () => {
  it('fmtWhen: time today, date and time otherwise', () => {
    vi.useFakeTimers({ toFake: ['Date'] })
    vi.setSystemTime(ny('2026-10-21T15:00'))
    expect(fmtWhen(ny('2026-10-21T09:31'))).toBe('09:31')
    expect(fmtWhen(ny('2026-10-20T16:00'))).toBe('Oct 20 16:00')
    expect(fmtWhen(ny('2026-10-21T09:31'), 'IL')).toBe('16:31')
  })
  it('ago', () => {
    vi.useFakeTimers({ toFake: ['Date'] })
    const now = ny('2026-10-21T15:00')
    vi.setSystemTime(now)
    expect(ago(null)).toBe('never')
    expect(ago(now - 5_000)).toBe('5s ago')
    expect(ago(now - 5 * 60_000)).toBe('5m ago')
    expect(ago(now - 3 * 3600_000)).toBe('3h ago')
    expect(ago(now - 2 * 86400_000)).toBe('2d ago')
  })
  it('helpers', () => {
    expect(inMin(5)).toBe('5m')
    expect(inMin(125)).toBe('2h 05m')
    const at = Date.parse('2026-10-21T09:40:00-04:00')
    expect(outage(Date.parse('2026-10-21T00:16:00-04:00'), at)).toBe('since 00:16 ET (9h 24m)')
    expect(outage(at + 5000, at)).toMatch(/\(0m\)$/)
    expect(fmtDay('2026-10-19')).toBe('Oct 19')
    expect(fmtTime(ny('2026-10-21T00:05'), 'NY')).toBe('00:05')
  })
})
