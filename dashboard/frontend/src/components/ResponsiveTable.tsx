import type { ReactNode } from 'react'
import { Td, Th } from './ui'

/**
 * One column definition, two layouts: a table from `md` up, a card per row below it (a
 * 390 px phone can't show 6–9 columns). On the card, `card` places the column:
 *   'title'  first line, left (joined with a space)
 *   'aside'  first line, right: the status a human looks for first (✓ / mismatch / P&L)
 *   'body'   label: value grid underneath (the default)
 *   false    table only
 */
export interface Col<T> {
  key: string
  head: ReactNode
  cell: (row: T) => ReactNode
  right?: boolean
  className?: string | ((row: T) => string)
  card?: 'title' | 'aside' | 'body' | false
  label?: string   // card label when `head` isn't plain text
}

const cls = <T,>(c: Col<T>, row: T) => (typeof c.className === 'function' ? c.className(row) : c.className ?? '')

export function ResponsiveTable<T>({ rows, cols, rowKey, rowClass, label }: {
  rows: T[]; cols: Col<T>[]; rowKey: (row: T, i: number) => string; rowClass?: (row: T) => string; label: string
}) {
  const title = cols.filter(c => c.card === 'title')
  const aside = cols.filter(c => c.card === 'aside')
  const body = cols.filter(c => (c.card ?? 'body') === 'body')
  return (
    <>
      <div className="hidden overflow-x-auto md:block">
        <table aria-label={label} className="w-full border-collapse">
          <thead><tr>{cols.map(c => <Th key={c.key} right={c.right}>{c.head}</Th>)}</tr></thead>
          <tbody>
            {rows.map((r, i) => (
              <tr key={rowKey(r, i)} className={rowClass?.(r) ?? ''}>
                {cols.map(c => <Td key={c.key} right={c.right} className={cls(c, r)}>{c.cell(r)}</Td>)}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <ul aria-label={label} className="m-0 flex list-none flex-col gap-2 p-3 md:hidden">
        {rows.map((r, i) => (
          <li key={rowKey(r, i)} className={`flex flex-col gap-2 rounded-[10px] border border-line bg-bg px-3.5 py-3 ${rowClass?.(r) ?? ''}`}>
            {(title.length > 0 || aside.length > 0) && (
              <div className="flex items-start gap-3">
                <div className="flex min-w-0 flex-1 flex-wrap items-baseline gap-x-2 gap-y-0.5">
                  {title.map(c => <span key={c.key} className={`min-w-0 break-words text-sm ${cls(c, r)}`}>{c.cell(r)}</span>)}
                </div>
                {aside.map(c => <span key={c.key} className={`shrink-0 text-right text-sm ${cls(c, r)}`}>{c.cell(r)}</span>)}
              </div>
            )}
            {body.length > 0 && (
              <dl className="m-0 grid grid-cols-2 gap-x-3 gap-y-1.5">
                {body.map(c => (
                  <div key={c.key} className="min-w-0">
                    <dt className="text-[11px] text-muted">{c.label ?? c.head}</dt>
                    <dd className={`m-0 break-words text-[13px] ${cls(c, r)}`}>{c.cell(r)}</dd>
                  </div>
                ))}
              </dl>
            )}
          </li>
        ))}
      </ul>
    </>
  )
}
