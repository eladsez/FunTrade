import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { screen, within } from '@testing-library/react'
import App from './App'
import { renderWithApi } from './test/render'
import { NOW, overview, session, system } from './test/fixtures'

beforeEach(() => {
  vi.useFakeTimers({ toFake: ['Date'] })
  vi.setSystemTime(NOW)
})
afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

const api = (over = {}) => ({ '/api/overview': overview(over), '/api/session': session, '/api/system': system() })

describe('App shell', () => {
  it('header: clocks in both zones; the dashboard is offline until its stream connects', async () => {
    renderWithApi(<App />, api(), { path: '/system' })
    const header = screen.getByRole('banner')
    expect(header.textContent).toContain('NY 14:05 · IL 21:05')
    expect(within(header).getByText('Dashboard offline')).toBeTruthy()
    expect(await screen.findByRole('heading', { name: 'Controls' })).toBeTruthy()
  })

  it('the phone nav marks System when a basket is halted', async () => {
    renderWithApi(<App />, api({ halted: [{ id: 'cms_ed_ppl', reason: 'x' }] }), { path: '/nowhere' })
    expect(await screen.findByLabelText('needs attention')).toBeTruthy()
    expect(screen.getByText('Not found.')).toBeTruthy()
  })
})
