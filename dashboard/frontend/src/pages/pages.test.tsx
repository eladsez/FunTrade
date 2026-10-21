import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import Overview from './Overview'
import Basket from './Basket'
import Orders from './Orders'
import System from './System'
import { renderWithApi } from '../test/render'
import { NOW, detail, orders, overview, session, system } from '../test/fixtures'

beforeEach(() => {
  vi.useFakeTimers({ toFake: ['Date'] })
  vi.setSystemTime(NOW)
})
afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('Overview', () => {
  it('shows the session from the market calendar', async () => {
    renderWithApi(<Overview />, { '/api/overview': overview(), '/api/session': session })
    const line = await screen.findByRole('region', { name: 'Session' })
    await within(line).findByText('US session open')
    expect(line.textContent).toContain('MOC cutoff 15:50 ET · 22:50 IL')
    expect(line.textContent).toContain('in 1h 45m')
  })

  it('names a holiday and an early close', async () => {
    const thanksgiving = { ...session, open: false, today_is_session: false, holiday: 'Thanksgiving Day', early_close: true,
      day: '2026-11-27', open_ms: Date.parse('2026-11-27T09:30-05:00'), close_ms: Date.parse('2026-11-27T13:00-05:00'),
      cutoff_ms: Date.parse('2026-11-27T12:50-05:00') }
    vi.setSystemTime(Date.parse('2026-11-26T11:00-05:00'))
    renderWithApi(<Overview />, { '/api/overview': overview(), '/api/session': thanksgiving })
    const line = await screen.findByRole('region', { name: 'Session' })
    await within(line).findByText('US market closed: Thanksgiving Day')
    expect(line.textContent).toContain('Opens Fri 09:30 ET')
    expect(line.textContent).toContain('Early close 13:00 ET (MOC cutoff 12:50)')
  })

  it('a difference seen once is "rechecking", not a red mismatch', async () => {
    renderWithApi(<Overview />, { '/api/overview': overview(), '/api/session': session })
    const alerts = await screen.findByRole('region', { name: 'Alerts' })
    expect(alerts.textContent).toContain('Rechecking IP')
    expect(alerts.textContent).not.toContain('broker 0')   // no red books-vs-broker banner
  })

  it('a confirmed mismatch is an alert', async () => {
    const m = { symbol: 'TRV', bot: 2, broker: 0, baskets: {}, match: false, settling: false }
    renderWithApi(<Overview />, { '/api/overview': overview({ mismatches: [m], settling: [] }), '/api/session': session })
    expect(await screen.findByText('TRV: books 2, broker 0')).toBeTruthy()
  })

  it('says what open P&L is priced with, and compares fills with the close', async () => {
    renderWithApi(<Overview />, { '/api/overview': overview(), '/api/session': session })
    expect(await screen.findByText(/5 baskets holding · delayed quotes, oldest 13:48 ET/)).toBeTruthy()
    expect(screen.getByText(/commissions \$15.60/)).toBeTruthy()
    const card = screen.getByRole('heading', { name: 'Paper vs research' }).closest('section')!
    expect(card.textContent).toContain('+3.2 bps')
    expect(card.textContent).toContain('+$12.40')
    expect(card.textContent).toContain('worst: HBAN in ajg_hban_schw, +18.7 bps')
    const fills = screen.getByRole('table', { name: 'Recent fills' })
    expect(within(fills).getByText('+18.7 bps').className).toContain('text-bad')
  })

  it('a fill whose close is not known yet says so', async () => {
    const f = { ...overview().fills[0], close: null, cost_usd: null, cost_bps: null }
    renderWithApi(<Overview />, { '/api/overview': overview({ fills: [f] }), '/api/session': session })
    expect((await screen.findAllByText('close pending')).length).toBe(2)   // table + card
  })
})

describe('Basket', () => {
  const api = { '/api/baskets/ajg_hban_schw': detail(), '/api/session': session }
  const open = (a = api) => renderWithApi(<Basket />, a, { path: '/baskets/ajg_hban_schw', route: '/baskets/:id' })

  it('shows the entry z, its decision day, held trading days and P&L after fees', async () => {
    open()
    expect(await screen.findByText('−2.37')).toBeTruthy()
    expect(screen.getByText('z of Oct 19 · in Oct 20 · restored')).toBeTruthy()
    expect(screen.getByText('1d')).toBeTruthy()
    expect(screen.getByText('trading days')).toBeTruthy()
    expect(screen.getByText('fees $3.12')).toBeTruthy()   // SCHW has no price: no net figure
  })

  it('net P&L once every leg is priced', async () => {
    const d = detail()
    d.legs[2] = { ...d.legs[2], price: 70.1, source: 'close', price_day: '2026-10-20', unrealized: 0 }
    open({ ...api, '/api/baskets/ajg_hban_schw': d })
    expect(await screen.findByText('+$37 after $3.12 fees')).toBeTruthy()
  })

  it('labels every leg price and the broker check', async () => {
    open()
    const legs = await screen.findByRole('table', { name: 'Legs' })
    expect(within(legs).getByText('delayed · 13:50 ET')).toBeTruthy()
    expect(within(legs).getByText('close Oct 20')).toBeTruthy()
    expect(within(legs).getByText('no price')).toBeTruthy()
    expect(within(legs).getByText('rechecking').className).toContain('text-warn')
    expect(within(legs).getByText('MISMATCH').className).toContain('text-bad')
  })

  it('shows fills against the close', async () => {
    open()
    const trades = await screen.findByRole('table', { name: 'Trades' })
    expect(within(trades).getByText(/SELL 133 HBAN @ 15.98 \(close 16.01, \+18.7 bps\)/)).toBeTruthy()
    expect(screen.getByText(/fills vs the close:/).textContent).toContain('over 3 (+1 pending)')
  })

  it('without an entry record, dashes', async () => {
    open({ ...api, '/api/baskets/ajg_hban_schw': detail({ entry: null, held_days: null, commissions: null }) })
    await screen.findByRole('table', { name: 'Legs' })
    expect(screen.queryByText(/fees/)).toBeNull()
    expect(screen.getAllByText('—').length).toBeGreaterThan(1)
  })
})

describe('Orders', () => {
  it('manual trades are shown neutrally; the bot\'s unbooked fills are flagged', async () => {
    renderWithApi(<Orders />, { '/api/orders': orders() })
    await userEvent.click(await screen.findByRole('tab', { name: /Executions today/ }))
    const t = screen.getByRole('table', { name: 'Executions today' })
    expect(within(t).getByText('manual trade').className).toContain('text-muted')
    expect(within(t).getByText('not booked').className).toContain('text-bad')
    expect(within(t).getByText('$1.04')).toBeTruthy()
    expect(screen.getByText("1 of the bot's not booked · 1 manual")).toBeTruthy()
  })

  it('positions: rechecking vs mismatch', async () => {
    renderWithApi(<Orders />, { '/api/orders': orders({ position_settling: [orders().positions[1]] }) })
    await userEvent.click(await screen.findByRole('tab', { name: /Positions/ }))
    const t = screen.getByRole('table', { name: 'Positions' })
    expect(within(t).getByText('Rechecking').className).toContain('text-warn')
    expect(within(t).getByText('Mismatch').className).toContain('text-bad')
    expect(screen.getByText('1 rechecking')).toBeTruthy()
  })
})

describe('System', () => {
  it('shows per-symbol cap usage, fullest first, expandable', async () => {
    renderWithApi(<System />, { '/api/system': system() })
    const meters = await screen.findAllByRole('meter')
    expect(meters).toHaveLength(8)
    expect(meters[0].getAttribute('aria-label')).toBe('S0 cap used')
    expect(meters[0].getAttribute('aria-valuenow')).toBe('97')
    await userEvent.click(screen.getByRole('button', { name: 'All 10' }))
    expect(screen.getAllByRole('meter')).toHaveLength(10)
  })

  it('an older bot without symbol usage says so', async () => {
    renderWithApi(<System />, { '/api/system': system({ symbol_caps: null }) })
    expect(await screen.findByText("This bot version doesn't report per-symbol usage.")).toBeTruthy()
  })

  it('warnings: the current run open with repeats folded, earlier runs collapsed', async () => {
    renderWithApi(<System />, { '/api/system': system() })
    expect(await screen.findByText(/Current run, started/)).toBeTruthy()
    expect(screen.getByText('×2')).toBeTruthy()
    const older = screen.getByText(/^Run, started .* · 1 distinct$/).closest('details')!
    expect(older.open).toBe(false)
    expect(within(older).getByText(/MISMATCH IP/)).toBeTruthy()
  })

  it('no warnings at all', async () => {
    renderWithApi(<System />, { '/api/system': system({ logs: [] }) })
    expect(await screen.findByText('No warnings or errors in the recent log.')).toBeTruthy()
  })
})
