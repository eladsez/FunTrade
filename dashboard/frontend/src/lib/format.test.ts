import { describe, expect, it } from 'vitest'
import { bps, held, money, pnlClass, price, qty, usd, z } from './format'

describe('usd', () => {
  it('formats dollars, signs and cents', () => {
    expect(usd(1234.4)).toBe('$1,234')
    expect(usd(-1234.4)).toBe('−$1,234')
    expect(usd(12, { sign: true })).toBe('+$12')
    expect(usd(3.155, { cents: true })).toBe('$3.16')
    expect(usd(0.2)).toBe('$0')
    expect(usd(0.004, { cents: true })).toBe('$0')
  })
  it('shows unknown as a dash, never as zero', () => {
    expect(usd(null)).toBe('—')
    expect(usd(undefined)).toBe('—')
    expect(usd(Number.NaN)).toBe('—')
  })
})

describe('numbers', () => {
  it('z with an explicit sign', () => {
    expect(z(-0.25)).toBe('−0.25')
    expect(z(2)).toBe('+2.00')
    expect(z(0)).toBe('0.00')
    expect(z(null)).toBe('—')
  })
  it('quantities keep fractions only when there are any', () => {
    expect(qty(-133)).toBe('−133')
    expect(qty(2)).toBe('+2')
    expect(qty(2, false)).toBe('2')
    expect(qty(0.5)).toBe('+0.50')
    expect(qty(null)).toBe('—')
  })
  it('basis points', () => {
    expect(bps(3.21)).toBe('+3.2 bps')
    expect(bps(-1)).toBe('−1.0 bps')
    expect(bps(0)).toBe('0.0 bps')
    expect(bps(null)).toBe('—')
  })
  it('held days', () => {
    expect(held(1)).toBe('1 trading day')
    expect(held(3)).toBe('3 trading days')
    expect(held(3, true)).toBe('3d')
    expect(held(null)).toBe('—')
  })
  it('money in the account currency', () => {
    expect(money(253648.4, 'ILS')).toBe('₪253,648')
    expect(money(10, 'USD')).toBe('$10')
    expect(money(10, 'EUR')).toBe('EUR 10')
    expect(money(null, 'ILS')).toBe('—')
  })
  it('price and P&L colour', () => {
    expect(price(44.181)).toBe('44.18')
    expect(price(null)).toBe('—')
    expect(pnlClass(1)).toBe('text-good')
    expect(pnlClass(-1)).toBe('text-bad')
    expect(pnlClass(0.001)).toBe('text-muted')
    expect(pnlClass(null)).toBe('text-muted')
  })
})
