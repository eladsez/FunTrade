import type { ReactElement } from 'react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { MemoryRouter, Route, Routes } from 'react-router-dom'
import { render } from '@testing-library/react'
import { vi } from 'vitest'

/** A canned answer: JSON with status 200, or a function of the request (writes, errors). */
export type Answer = unknown | ((init: RequestInit | undefined) => { status: number; body: unknown })

/** Renders `ui` at `path` (matched against `route`) with fetch answering from `api`. */
export function renderWithApi(ui: ReactElement, api: Record<string, Answer>, { path = '/', route = '*' } = {}) {
  const fetch = vi.fn(async (url: string, init?: RequestInit) => {
    const a = api[url]
    const { status, body } = a === undefined ? { status: 404, body: { detail: 'not found' } }
      : typeof a === 'function' ? (a as (i?: RequestInit) => { status: number; body: unknown })(init) : { status: 200, body: a }
    return new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } })
  })
  vi.stubGlobal('fetch', fetch)
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false } } })
  const r = render(
    <QueryClientProvider client={qc}>
      <MemoryRouter initialEntries={[path]}><Routes><Route path={route} element={ui} /></Routes></MemoryRouter>
    </QueryClientProvider>,
  )
  return { ...r, fetch, qc }
}
