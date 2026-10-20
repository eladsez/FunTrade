const MINUS = '−'

/** $1,234 (cents only when asked). `sign` adds + for positives. null → — */
export function usd(v: number | null | undefined, { sign = false, cents }: { sign?: boolean; cents?: boolean } = {}) {
  if (v == null || Number.isNaN(v)) return '—'
  const digits = cents ? 2 : 0
  const s = Math.abs(v).toLocaleString('en-US', { minimumFractionDigits: digits, maximumFractionDigits: digits })
  if (Math.abs(v) < (cents ? 0.005 : 0.5)) return '$0'
  return `${v < 0 ? MINUS : sign ? '+' : ''}$${s}`
}

export function money(v: number | null | undefined, currency: string) {
  if (v == null) return '—'
  const sym = currency === 'ILS' ? '₪' : currency === 'USD' ? '$' : `${currency} `
  return `${sym}${Math.round(v).toLocaleString('en-US')}`
}

export function z(v: number | null | undefined) {
  if (v == null) return '—'
  return `${v < 0 ? MINUS : v > 0 ? '+' : ''}${Math.abs(v).toFixed(2)}`
}

export function qty(v: number | null | undefined, sign = true) {
  if (v == null) return '—'
  const n = Math.abs(v) % 1 === 0 ? Math.abs(v).toFixed(0) : Math.abs(v).toFixed(2)
  return `${v < 0 ? MINUS : sign && v > 0 ? '+' : ''}${n}`
}

export const price = (v: number | null | undefined) => (v == null ? '—' : v.toFixed(2))

export const pnlClass = (v: number | null | undefined) =>
  v == null || Math.abs(v) < 0.005 ? 'text-muted' : v > 0 ? 'text-good' : 'text-bad'
