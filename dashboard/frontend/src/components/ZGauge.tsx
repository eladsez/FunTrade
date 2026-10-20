import { z as fmtZ } from '../lib/format'

const pct = (v: number) => `${((Math.max(-3, Math.min(3, v)) + 3) / 6) * 100}%`

/** z on a −3…+3 track with the basket's entry lines (blue = enter long, orange = enter short)
 *  and exit band (grey). */
export function ZGauge({ z, entry, exit, compact }: { z: number | null; entry: number; exit: number; compact?: boolean }) {
  return (
    <div className="flex items-center gap-2.5">
      <div className={`relative ${compact ? 'h-3.5 flex-1' : 'h-[18px] w-[150px]'}`} role="img"
        aria-label={`z ${fmtZ(z)}, enter at ±${entry.toFixed(2)}, exit at ±${exit.toFixed(2)}`}>
        <div className="absolute inset-x-0 top-1/2 h-0.5 -translate-y-1/2 bg-line" />
        <div className="absolute top-[3px] bottom-[3px] w-px bg-warn" style={{ left: pct(entry) }} />
        <div className="absolute top-[3px] bottom-[3px] w-px bg-accent" style={{ left: pct(-entry) }} />
        <div className="absolute top-[5px] bottom-[5px] w-px bg-[#5A6472]" style={{ left: pct(exit) }} />
        <div className="absolute top-[5px] bottom-[5px] w-px bg-[#5A6472]" style={{ left: pct(-exit) }} />
        {z != null && (
          <div className={`absolute top-1/2 -translate-x-1/2 -translate-y-1/2 rounded-full bg-text ${compact ? 'size-2' : 'size-2.5'}`} style={{ left: pct(z) }} />
        )}
      </div>
      <span className={`num text-right ${compact ? 'w-11 text-[13px]' : 'w-12 text-sm'}`}>{fmtZ(z)}</span>
    </div>
  )
}

export function ZLegend() {
  return (
    <span className="flex flex-wrap gap-3.5 text-[13px] text-muted">
      <span className="flex items-center gap-1.5"><span className="h-3 w-px bg-accent" />enter long (z below −entry)</span>
      <span className="flex items-center gap-1.5"><span className="h-3 w-px bg-warn" />enter short (z above +entry)</span>
      <span className="flex items-center gap-1.5"><span className="h-2 w-px bg-[#5A6472]" />exit band</span>
    </span>
  )
}
