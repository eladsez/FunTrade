import { fmtDay } from '../lib/time'
import { z as fmtZ } from '../lib/format'
import type { Close } from '../lib/types'

const LIM = 3
// Days from the startup z history carry no position (null): only journaled days are drawn as held.
const held_ = (c: Close) => (c.position === 'LONG' || c.position === 'SHORT') && c.state !== 'FLAT'

/** Daily close z with the entry zones shaded, the exit band, and the days in position
 *  highlighted (green). Values beyond ±3 are drawn at the edge. */
export function ZChart({ closes, entry, exit, height = 262 }: { closes: Close[]; entry: number; exit: number; height?: number }) {
  const W = 1000
  const H = 262
  const y = (v: number) => H / 2 - (Math.max(-LIM, Math.min(LIM, v)) / LIM) * (H / 2)
  const pts = closes.filter(c => c.z != null)
  const x = (i: number) => (pts.length > 1 ? (i / (pts.length - 1)) * W : W / 2)

  let line = ''
  let held = ''
  const entries: { i: number; side: string }[] = []
  pts.forEach((c, i) => {
    line += `${i ? 'L' : 'M'}${x(i).toFixed(1)} ${y(c.z!).toFixed(1)} `
    const inPos = held_(c)
    const prevIn = i > 0 && held_(pts[i - 1])
    if (inPos) held += `${prevIn ? 'L' : 'M'}${x(i).toFixed(1)} ${y(c.z!).toFixed(1)} `
    if (c.action === 'ENTER_LONG' || c.action === 'ENTER_SHORT') entries.push({ i, side: c.action })
  })

  if (pts.length < 2) {
    return <div className="flex items-center justify-center text-sm text-muted" style={{ height }}>No z history yet. The bot writes it when it starts.</div>
  }
  const ticks = [0, 0.2, 0.4, 0.6, 0.8, 1].map(f => pts[Math.round(f * (pts.length - 1))].day)
  const last = pts[pts.length - 1]
  return (
    <div className="flex gap-2">
      <div className="num flex w-7 flex-col justify-between pb-[18px] text-right text-[11px] text-muted" style={{ height: height + 18 }}>
        {['+3', '+2', '+1', '0', '−1', '−2', '−3'].map(t => <span key={t}>{t}</span>)}
      </div>
      <div className="flex min-w-0 flex-1 flex-col gap-1">
        <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="block w-full" style={{ height }} role="img"
          aria-label={`z-score over ${pts.length} trading days, latest ${fmtZ(last.z)} on ${last.day}`}>
          <rect x={0} y={0} width={W} height={y(entry)} fill="#2A2116" />
          <rect x={0} y={y(-entry)} width={W} height={H - y(-entry)} fill="#14213A" />
          <line x1={0} x2={W} y1={y(entry)} y2={y(entry)} stroke="#F2A65A" strokeDasharray="6 4" vectorEffect="non-scaling-stroke" />
          <line x1={0} x2={W} y1={y(-entry)} y2={y(-entry)} stroke="#6CA8FF" strokeDasharray="6 4" vectorEffect="non-scaling-stroke" />
          <line x1={0} x2={W} y1={y(exit)} y2={y(exit)} stroke="#3A4450" vectorEffect="non-scaling-stroke" />
          <line x1={0} x2={W} y1={y(-exit)} y2={y(-exit)} stroke="#3A4450" vectorEffect="non-scaling-stroke" />
          <line x1={0} x2={W} y1={H / 2} y2={H / 2} stroke="#2A323D" vectorEffect="non-scaling-stroke" />
          <path d={line} fill="none" stroke="#E6EAF0" strokeWidth={2} vectorEffect="non-scaling-stroke" />
          {held && <path d={held} fill="none" stroke="#7FE0AE" strokeWidth={3} vectorEffect="non-scaling-stroke" />}
          {entries.map(e => (
            <line key={e.i} x1={x(e.i)} x2={x(e.i)} y1={0} y2={H} stroke={e.side === 'ENTER_LONG' ? '#6CA8FF' : '#F2A65A'} strokeOpacity={0.5} vectorEffect="non-scaling-stroke" />
          ))}
        </svg>
        <div className="num flex justify-between text-[11px] text-muted">
          {ticks.map((d, i) => <span key={i} className={i % 2 ? 'hidden sm:inline' : ''}>{fmtDay(d)}</span>)}
        </div>
      </div>
    </div>
  )
}

export function ZChartLegend() {
  return (
    <div className="flex flex-wrap gap-[18px] text-[13px] text-dim">
      <span className="flex items-center gap-1.5"><span className="h-0.5 w-3.5 bg-text" />z at the close</span>
      <span className="flex items-center gap-1.5"><span className="h-[3px] w-3.5 bg-good" />in position</span>
      <span className="flex items-center gap-1.5"><span className="h-2.5 w-3.5 border-t border-dashed border-accent bg-[#14213A]" />enter long zone</span>
      <span className="flex items-center gap-1.5"><span className="h-2.5 w-3.5 border-b border-dashed border-warn bg-[#2A2116]" />enter short zone</span>
      <span className="flex items-center gap-1.5"><span className="h-3 w-px bg-accent opacity-60" />entry decided</span>
    </div>
  )
}
