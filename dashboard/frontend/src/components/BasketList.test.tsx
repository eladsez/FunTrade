import { describe, expect, it } from 'vitest'
import { render, screen, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { MemoryRouter } from 'react-router-dom'
import { BasketList, note } from './BasketList'
import { row } from '../test/fixtures'

describe('note', () => {
  it('says what a human wants to know', () => {
    expect(note(row({ state: 'SYSTEM_HALTED', halt_reason: 'broker position differs' }))).toBe('broker position differs')
    expect(note(row({ state: 'SYSTEM_HALTED', halt_reason: null }))).toBe('Halted: needs a human')
    expect(note(row({ skipped_today: 'SCHW net > cap' }))).toBe('Entry skipped: SCHW net > cap')
    expect(note(row({ state: 'ENTRY_PENDING', working_legs: 3 }))).toBe('MOC entry working · 3 legs')
    expect(note(row({ state: 'EXIT_PENDING', working_legs: 2 }))).toBe('MOC exit working · 2 legs')
    expect(note(row({ state: 'RECOVERY_PENDING' }))).toBe('Unbalanced: recovery sweep at next session')
    expect(note(row({ pending_action: 'ENTER_LONG' }))).toBe('enter long at next session')
    expect(note(row({ state: 'FLAT', in_books: false, position: 'FLAT', close_z: 1.8 }))).toBe('Near entry (±2.00)')
    expect(note(row({ state: 'FLAT', in_books: false, position: 'FLAT', close_z: 0.1 }))).toBe('')
  })
  it('words the exit with the sign of exit_z (a negative exit_z once printed "−−0.25")', () => {
    expect(note(row({ position: 'LONG', exit_z: 0.25 }))).toBe('Exits when z rises above −0.25')
    expect(note(row({ position: 'LONG', exit_z: -0.25 }))).toBe('Exits when z rises above +0.25')
    expect(note(row({ position: 'SHORT', exit_z: 0.25 }))).toBe('Exits when z falls below +0.25')
  })
})

describe('BasketList', () => {
  const rows = [
    row(),
    row({ id: 'cms_ed_ppl', tickers: ['CMS', 'ED', 'PPL'], state: 'SYSTEM_HALTED', position: 'FLAT', in_books: false, held_days: null }),
    row({ id: 'aapl_mu_wdc', tickers: ['AAPL', 'MU', 'WDC'], state: 'FLAT', position: 'FLAT', in_books: false, close_z: 0.2, held_days: null }),
  ]
  const renderList = () => render(<MemoryRouter><BasketList rows={rows} initial="all" /></MemoryRouter>)

  it('ranks halted first and shows held trading days', () => {
    renderList()
    const table = screen.getByRole('table')
    const ids = within(table).getAllByRole('link').map(a => a.textContent)
    expect(ids).toEqual(['cms_ed_ppl', 'ajg_hban_schw', 'aapl_mu_wdc'])
    expect(within(table).getByText('1d')).toBeTruthy()
  })

  it('filters and searches', async () => {
    renderList()
    await userEvent.click(screen.getByRole('button', { name: /Halted/ }))
    expect(within(screen.getByRole('table')).getAllByRole('link').map(a => a.textContent)).toEqual(['cms_ed_ppl'])
    await userEvent.click(screen.getByRole('button', { name: /All/ }))
    await userEvent.type(screen.getByRole('searchbox'), 'mu')
    expect(within(screen.getByRole('table')).getAllByRole('link').map(a => a.textContent)).toEqual(['aapl_mu_wdc'])
  })
})
