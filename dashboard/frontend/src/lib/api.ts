import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type { BasketDetail, Orders, Overview, System } from './types'

export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
}

async function call<T>(path: string, init?: RequestInit): Promise<T> {
  const r = await fetch(path, init)
  if (!r.ok) {
    let msg = r.statusText
    try { msg = (await r.json()).detail ?? msg } catch { /* not JSON */ }
    throw new ApiError(r.status, msg)
  }
  return r.json()
}

// Writes carry the app's header: the server refuses writes without it (no cross-site forms).
const post = <T,>(path: string, body: unknown) =>
  call<T>(path, { method: 'POST', headers: { 'content-type': 'application/json', 'x-funtrade': '1' }, body: JSON.stringify(body) })

// Data is pushed (lib/live.tsx invalidates on change); polling is only a fallback.
const opts = { refetchInterval: 60_000, staleTime: 5_000 }

export const useOverview = () => useQuery({ queryKey: ['overview'], queryFn: () => call<Overview>('/api/overview'), ...opts })
export const useBasket = (id: string) => useQuery({ queryKey: ['basket', id], queryFn: () => call<BasketDetail>(`/api/baskets/${id}`), ...opts })
export const useOrders = () => useQuery({ queryKey: ['orders'], queryFn: () => call<Orders>('/api/orders'), ...opts })
export const useSystem = () => useQuery({ queryKey: ['system'], queryFn: () => call<System>('/api/system'), ...opts })

export function useSetFlag() {
  const qc = useQueryClient()
  return useMutation({
    mutationFn: (v: { flag: 'kill' | 'flatten'; on: boolean; confirm: string }) =>
      post<{ on: boolean }>(`/api/controls/${v.flag}`, { on: v.on, confirm: v.confirm }),
    onSuccess: () => qc.invalidateQueries(),
  })
}

export function useClearHalt() {
  const qc = useQueryClient()
  return useMutation({
    mutationFn: (v: { basket: string; confirm: string; remove_books: boolean }) =>
      post<{ removed_keys: string[]; backup: string }>(`/api/baskets/${v.basket}/clear-halt`, { confirm: v.confirm, remove_books: v.remove_books }),
    onSuccess: () => qc.invalidateQueries(),
  })
}
