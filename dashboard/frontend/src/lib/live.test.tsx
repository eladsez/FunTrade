import { afterEach, describe, expect, it, vi } from 'vitest'
import { act, render, screen } from '@testing-library/react'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { LiveProvider, useStream } from './live'

class FakeEventSource {
  static last: FakeEventSource
  onopen: (() => void) | null = null
  onerror: (() => void) | null = null
  listeners: Record<string, (e: MessageEvent) => void> = {}
  closed = false
  url: string
  constructor(url: string) {
    this.url = url
    FakeEventSource.last = this
  }
  addEventListener(kind: string, f: (e: MessageEvent) => void) { this.listeners[kind] = f }
  close() { this.closed = true }
  emit(kind: string, data = '{}') {
    act(() => { this.listeners[kind]?.(new MessageEvent(kind, { data })) })
  }
}

function Probe() {
  const { connected, live } = useStream()
  return <span>{connected ? 'up' : 'down'} {live ? Object.keys(live.prices).join(',') : 'no-live'}</span>
}

afterEach(() => {
  vi.unstubAllGlobals()
  vi.useRealTimers()
})

function setup() {
  vi.stubGlobal('EventSource', FakeEventSource)
  const qc = new QueryClient()
  const spy = vi.spyOn(qc, 'invalidateQueries')
  const r = render(<QueryClientProvider client={qc}><LiveProvider><Probe /></LiveProvider></QueryClientProvider>)
  return { es: FakeEventSource.last, spy, ...r }
}

describe('LiveProvider', () => {
  it('connects to /api/events and tracks the connection', () => {
    const { es } = setup()
    expect(es.url).toBe('/api/events')
    expect(screen.getByText(/down/)).toBeTruthy()
    act(() => es.onopen?.())
    expect(screen.getByText(/up/)).toBeTruthy()
    act(() => es.onerror?.())
    expect(screen.getByText(/down/)).toBeTruthy()
  })

  it('refetches everything after a reconnect, not on the first connect', () => {
    const { es, spy } = setup()
    act(() => es.onopen?.())
    expect(spy).not.toHaveBeenCalled()
    act(() => es.onopen?.())
    expect(spy).toHaveBeenCalledWith()
  })

  it('state → everything; broker → the broker-backed views', () => {
    const { es, spy } = setup()
    es.emit('state')
    expect(spy).toHaveBeenLastCalledWith()
    spy.mockClear()
    es.emit('broker')
    expect(spy.mock.calls.map(c => c[0]?.queryKey?.[0])).toEqual(['orders', 'overview', 'system', 'basket'])
  })

  it('live pushes prices, refetching P&L at most every 10 s, and ignores garbage', () => {
    vi.useFakeTimers({ toFake: ['Date'] })
    vi.setSystemTime(100_000)
    const { es, spy } = setup()
    const msg = JSON.stringify({ connected: true, baskets: {}, prices: { HBAN: 15.7 } })
    es.emit('live', msg)
    expect(screen.getByText(/HBAN/)).toBeTruthy()
    expect(spy).toHaveBeenCalledTimes(2)
    es.emit('live', msg)
    expect(spy).toHaveBeenCalledTimes(2)
    vi.setSystemTime(111_000)
    es.emit('live', msg)
    expect(spy).toHaveBeenCalledTimes(4)
    es.emit('live', 'not json')
    expect(screen.getByText(/HBAN/)).toBeTruthy()
  })

  it('closes the stream on unmount', () => {
    const { es, unmount } = setup()
    unmount()
    expect(es.closed).toBe(true)
  })
})
