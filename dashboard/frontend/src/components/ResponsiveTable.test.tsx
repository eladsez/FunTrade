import { describe, expect, it } from 'vitest'
import { render, screen, within } from '@testing-library/react'
import { ResponsiveTable, type Col } from './ResponsiveTable'

interface R { sym: string; qty: number; ok: boolean; ref: string }
const cols: Col<R>[] = [
  { key: 'sym', head: 'Symbol', cell: r => r.sym, card: 'title' },
  { key: 'qty', head: 'Qty', right: true, cell: r => String(r.qty), className: r => (r.qty < 0 ? 'neg' : 'pos') },
  { key: 'ok', head: <b>Status</b>, label: 'Status', cell: r => (r.ok ? 'match' : 'MISMATCH'), card: 'aside' },
  { key: 'ref', head: 'orderRef', cell: r => r.ref, card: false },
]
const rows: R[] = [{ sym: 'HBAN', qty: -133, ok: true, ref: 'a#1' }, { sym: 'IP', qty: 53, ok: false, ref: 'b#2' }]

describe('ResponsiveTable', () => {
  it('renders a table with every column', () => {
    render(<ResponsiveTable label="Legs" rows={rows} cols={cols} rowKey={r => r.sym} rowClass={r => (r.ok ? '' : 'bad')} />)
    const table = screen.getByRole('table', { name: 'Legs' })
    expect(within(table).getAllByRole('columnheader').map(h => h.textContent)).toEqual(['Symbol', 'Qty', 'Status', 'orderRef'])
    const [, first, second] = within(table).getAllByRole('row')
    expect(within(first).getAllByRole('cell').map(c => c.textContent)).toEqual(['HBAN', '-133', 'match', 'a#1'])
    expect(second.className).toBe('bad')
    expect(within(first).getAllByRole('cell')[1].className).toContain('neg')
  })

  it('renders a card per row: title and status first, the rest labelled, table-only columns left out', () => {
    render(<ResponsiveTable label="Legs" rows={rows} cols={cols} rowKey={r => r.sym} />)
    const cards = within(screen.getByRole('list', { name: 'Legs' })).getAllByRole('listitem')
    expect(cards).toHaveLength(2)
    const ip = cards[1]
    expect(ip.textContent).toContain('IP')
    expect(ip.textContent).toContain('MISMATCH')
    expect(within(ip).getByText('Qty').tagName).toBe('DT')
    expect(within(ip).getByText('53').tagName).toBe('DD')
    expect(ip.textContent).not.toContain('b#2')
    expect(within(ip).queryByText('Status')).toBeNull()   // the aside needs no label
  })

  it('a card without title or aside columns has only the grid', () => {
    render(<ResponsiveTable label="Plain" rows={rows} cols={[cols[1]]} rowKey={r => r.sym} />)
    const card = within(screen.getByRole('list', { name: 'Plain' })).getAllByRole('listitem')[0]
    expect(card.children).toHaveLength(1)
  })
})
