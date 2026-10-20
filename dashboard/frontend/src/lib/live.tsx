import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from 'react'
import { useQueryClient } from '@tanstack/react-query'
import type { Live } from './types'

/**
 * One EventSource to /api/events. The server pushes
 *   state  → the bot's files or journal changed → refetch everything
 *   broker → a new broker snapshot              → refetch orders / overview / system
 *   live   → prices and intraday z from the bot  → kept here; P&L views refetch at most every 10 s
 * EventSource reconnects on its own; after a reconnect everything is refetched once.
 */
interface Stream {
  connected: boolean          // this browser ↔ dashboard
  live: Live | null           // the dashboard's view of the bot's live feed
}
const Ctx = createContext<Stream>({ connected: false, live: null })

export function LiveProvider({ children }: { children: ReactNode }) {
  const qc = useQueryClient()
  const [state, setState] = useState<Stream>({ connected: false, live: null })
  const wasConnected = useRef(false)
  const lastPnl = useRef(0)

  useEffect(() => {
    const es = new EventSource('/api/events')
    es.onopen = () => {
      if (wasConnected.current) qc.invalidateQueries()
      wasConnected.current = true
      setState(s => ({ ...s, connected: true }))
    }
    es.onerror = () => setState(s => ({ ...s, connected: false }))
    es.addEventListener('state', () => qc.invalidateQueries())
    es.addEventListener('broker', () => {
      for (const key of ['orders', 'overview', 'system', 'basket']) qc.invalidateQueries({ queryKey: [key] })
    })
    es.addEventListener('live', e => {
      try {
        const live = JSON.parse((e as MessageEvent).data) as Live
        setState(s => ({ ...s, live }))
        const now = Date.now()
        if (now - lastPnl.current > 10_000) {
          lastPnl.current = now
          qc.invalidateQueries({ queryKey: ['overview'] })
          qc.invalidateQueries({ queryKey: ['basket'] })
        }
      } catch { /* ignore a malformed push */ }
    })
    return () => es.close()
  }, [qc])

  return <Ctx.Provider value={state}>{children}</Ctx.Provider>
}

export const useStream = () => useContext(Ctx)
