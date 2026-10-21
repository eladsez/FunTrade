import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import Basket from './Basket'
import Baskets from './Baskets'
import Orders from './Orders'
import System from './System'
import { renderWithApi } from '../test/render'
import { NOW, detail, orders, overview, row, session, system } from '../test/fixtures'
import type { BasketDetail } from '../lib/types'

beforeEach(() => {
  vi.useFakeTimers({ toFake: ['Date'] })
  vi.setSystemTime(NOW)
})
afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

const halted = { id: 'cms_ed_ppl', reason: 'broker position in ED differs', halted_at: NOW - 3600_000, book_keys: { cms_ed_ppl_ED: 12 } }

describe('System controls', () => {
  it('kill switch: confirm by typing KILL, posted with the app header', async () => {
    const { fetch } = renderWithApi(<System />, {
      '/api/system': system(),
      '/api/controls/kill': () => ({ status: 200, body: { flag: 'kill', on: true } }),
    })
    await userEvent.click(await screen.findByRole('button', { name: 'Turn kill switch on' }))
    const dialog = screen.getByRole('dialog', { name: 'Turn kill switch on?' })
    const go = within(dialog).getByRole('button', { name: 'Turn on' }) as HTMLButtonElement
    expect(go.disabled).toBe(true)
    await userEvent.type(within(dialog).getByRole('textbox'), 'KILL')
    expect(go.disabled).toBe(false)
    await userEvent.click(go)
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull())
    const init = fetch.mock.calls.find(([u]) => u === '/api/controls/kill')![1]!
    expect(init.method).toBe('POST')
    expect((init.headers as Record<string, string>)['x-funtrade']).toBe('1')
    expect(JSON.parse(init.body as string)).toEqual({ on: true, confirm: 'KILL' })
  })

  it('a refused write shows the server\'s reason in the dialog', async () => {
    renderWithApi(<System />, {
      '/api/system': system({ flags: { kill: false, flatten: true } }),
      '/api/controls/flatten': () => ({ status: 400, body: { detail: 'confirmation word mismatch' } }),
    })
    await userEvent.click(await screen.findByRole('button', { name: 'Stop flattening' }))
    const dialog = screen.getByRole('dialog', { name: 'Stop flattening?' })
    await userEvent.type(within(dialog).getByRole('textbox'), 'FLATTEN{Enter}')
    expect(await within(dialog).findByText('confirmation word mismatch')).toBeTruthy()
  })

  it('Escape and Cancel close the dialog', async () => {
    renderWithApi(<System />, { '/api/system': system() })
    await userEvent.click(await screen.findByRole('button', { name: 'Flatten…' }))
    await userEvent.keyboard('{Escape}')
    expect(screen.queryByRole('dialog')).toBeNull()
    await userEvent.click(screen.getByRole('button', { name: 'Turn kill switch on' }))
    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }))
    expect(screen.queryByRole('dialog')).toBeNull()
  })

  it('clear halt waits for a stopped bot', async () => {
    renderWithApi(<System />, { '/api/system': system({ halts: [halted] }) })
    const btn = await screen.findByRole('button', { name: 'Clear halt' }) as HTMLButtonElement
    expect(btn.disabled).toBe(true)
    expect(screen.getByText('Available once the bot is stopped')).toBeTruthy()
  })

  it('clear halt with the bot stopped: books removed on request, backup reported', async () => {
    const { fetch } = renderWithApi(<System />, {
      '/api/system': system({ halts: [halted], can_edit_state: true }),
      '/api/baskets/cms_ed_ppl/clear-halt': () => ({ status: 200, body: { removed_keys: ['cms_ed_ppl_ED'], backup: 'dash/backups/1' } }),
    })
    await userEvent.click(await screen.findByRole('checkbox'))   // keep the books this time
    await userEvent.click(screen.getByRole('checkbox'))          // ...no, remove them
    await userEvent.click(screen.getByRole('button', { name: 'Clear halt' }))
    const dialog = screen.getByRole('dialog')
    expect(dialog.textContent).toContain('and its book entries (cms_ed_ppl_ED)')
    await userEvent.type(within(dialog).getByRole('textbox'), 'cms_ed_ppl')
    await userEvent.click(within(dialog).getByRole('button', { name: 'Clear halt' }))
    expect(await screen.findByText(/cms_ed_ppl cleared, removed cms_ed_ppl_ED\. Backup: dash\/backups\/1/)).toBeTruthy()
    const call = fetch.mock.calls.find(([u]) => u === '/api/baskets/cms_ed_ppl/clear-halt')!
    expect(JSON.parse(call[1]?.body as string)).toEqual({ confirm: 'cms_ed_ppl', remove_books: true })
  })

  it('health rows reflect the sources', async () => {
    renderWithApi(<System />, { '/api/system': system({ bot: { ...system().bot, state: 'silent' }, broker: { connected: false, checked_at: null, error: 'cannot connect' } }) })
    expect(await screen.findByText('Silent')).toBeTruthy()
    expect(screen.getByText('cannot connect')).toBeTruthy()
  })

  it('shows whether the Gateway is logged in to IBKR', async () => {
    const down = Date.parse('2026-10-21T00:16:00-04:00')
    const { unmount } = renderWithApi(<System />, { '/api/system': system({ ibkr: { connected: false, down_since: down } }) })
    const row = (await screen.findByText('Gateway ↔ IBKR')).parentElement!
    expect(row.textContent).toContain('Cut off (1100)')
    expect(row.textContent).toContain('since 00:16 ET')
    unmount()
    renderWithApi(<System />, { '/api/system': system() })
    expect((await screen.findByText('Gateway ↔ IBKR')).parentElement!.textContent).toContain('Logged in')
  })

  it('an unknown IBKR link says so', async () => {
    renderWithApi(<System />, { '/api/system': system({ ibkr: { connected: null, down_since: null } }) })
    expect((await screen.findByText('Gateway ↔ IBKR')).parentElement!.textContent).toContain('unknown')
  })

  it('a cut-off link without a start time leaves it out', async () => {
    renderWithApi(<System />, { '/api/system': system({ ibkr: { connected: false, down_since: null } }) })
    const row = (await screen.findByText('Gateway ↔ IBKR')).parentElement!
    expect(row.textContent).not.toContain('since')
    expect(row.textContent).toContain('restart ib-gateway')
  })

  it('a failed load says why', async () => {
    renderWithApi(<System />, { '/api/system': () => ({ status: 500, body: { detail: 'state unreadable' } }) })
    expect(await screen.findByText("Couldn't load: state unreadable")).toBeTruthy()
  })
})

describe('Orders, working orders', () => {
  it('rows that differ stand out; orders not placed by the bot are listed', async () => {
    const b = { symbol: 'HBAN', side: 'BUY', quantity: 242, type: 'MOC', status: 'PreSubmitted', order_ref: 'x#1' }
    renderWithApi(<Orders />, { '/api/orders': orders({
      orders: [
        { basket: 'hban_schw_trv', symbol: 'HBAN', side: 'BUY', quantity: 241, client_order_id: 'x#1', basket_state: 'ENTRY_PENDING', broker: b, match: 'differs' },
        { basket: 'hban_schw_trv', symbol: 'TRV', side: 'SELL', quantity: 5, client_order_id: 'x#2', basket_state: 'ENTRY_PENDING', broker: null, match: 'missing_at_broker' },
      ],
      other_orders: [{ symbol: 'SPY', side: 'BUY', quantity: 1, type: 'LMT', status: 'Submitted', order_ref: '' }],
    }) })
    const t = await screen.findByRole('table', { name: 'Working orders' })
    const rows = within(t).getAllByRole('row').slice(1)
    expect(rows.every(r => r.className.includes('bg-[#24141A]'))).toBe(true)
    expect(within(t).getByText('(broker 242)')).toBeTruthy()
    expect(within(t).getByText('not at broker')).toBeTruthy()
    expect(screen.getByText(/1 other open order at the broker not placed by the bot: BUY 1 SPY \(LMT, Submitted\)/)).toBeTruthy()
    expect(screen.getByText('2 differ')).toBeTruthy()
  })

  it('empty states, and a broker that is down', async () => {
    renderWithApi(<Orders />, { '/api/orders': orders({ positions: [], executions: [], broker: { connected: false, checked_at: null, error: 'Gateway restarting' } }) })
    expect(await screen.findByText(/No orders working/)).toBeTruthy()
    expect(screen.getByText(/not connected: Gateway restarting/)).toBeTruthy()
    await userEvent.click(screen.getByRole('tab', { name: /Positions/ }))
    expect(screen.getByText('Nothing held, by the bot or at the broker.')).toBeTruthy()
    await userEvent.click(screen.getByRole('tab', { name: /Executions/ }))
    expect(screen.getByText(/No executions yet today/)).toBeTruthy()
  })
})

describe('Basket, what happens next', () => {
  const open = (d: BasketDetail) => renderWithApi(<Basket />, { [`/api/baskets/${d.id}`]: d, '/api/session': session },
    { path: `/baskets/${d.id}`, route: '/baskets/:id' })

  it.each([
    [{ state: 'IN_POSITION' as const }, 'If z is above −0.25, the exit is decided.'],
    [{ state: 'IN_POSITION' as const, position: 'SHORT' as const }, 'If z is below +0.25, the exit is decided.'],
    [{ state: 'FLAT' as const, position: 'FLAT' as const }, 'Enters long if z < −2.00, short if z > +2.00.'],
    [{ state: 'ENTRY_PENDING' as const, working_legs: 3 }, '3 MOC orders fill in the closing auction.'],
    [{ state: 'EXIT_PENDING' as const, working_legs: 3 }, 'FLAT once every leg filled.'],
    [{ state: 'SYSTEM_HALTED' as const, halt_reason: 'broker differs' }, 'See System → Halted baskets.'],
    [{ state: 'RECOVERY_PENDING' as const }, 'The unbalanced legs are flattened at the next session.'],
  ])('%o', async (over, text) => {
    open(detail(over))
    expect(await screen.findByText(text)).toBeTruthy()
    expect((await screen.findAllByText(/Close 16:00 ET|Halted|Recovery/)).length).toBeGreaterThan(0)
  })

  it('range pills and intraday z', async () => {
    open(detail({ closes: Array.from({ length: 300 }, (_, i) => ({ day: `2026-01-${String((i % 28) + 1).padStart(2, '0')}`, z: Math.sin(i), action: null, position: null, state: null })) }))
    await userEvent.click(await screen.findByRole('button', { name: '30d' }))
    expect(screen.getByRole('button', { name: '30d' }).getAttribute('aria-pressed')).toBe('true')
    await userEvent.click(screen.getByRole('button', { name: 'All' }))
    expect(screen.getByRole('button', { name: 'All' }).getAttribute('aria-pressed')).toBe('true')
  })

  it('no trades yet, a skipped entry, and an unknown basket', async () => {
    open(detail({ cycles: [], skipped: [{ t: NOW - 60_000, reason: 'SCHW net > cap' }] }))
    expect(await screen.findByText('No trades journaled for this basket yet.')).toBeTruthy()
    expect(screen.getByText(/Last skipped entry: .* SCHW net > cap/)).toBeTruthy()
  })
})

describe('Baskets page', () => {
  it('lists every basket', async () => {
    renderWithApi(<Baskets />, { '/api/overview': overview({ baskets: [row(), row({ id: 'b2', tickers: ['X', 'Y'] })] }) })
    expect((await screen.findAllByText('b2')).length).toBeGreaterThan(0)
  })
})
