import { useEffect, useId, useRef, useState, type ReactNode } from 'react'

/** Type-to-confirm dialog: a bottom sheet on the phone, centered on wider screens. */
export function Confirm({ title, word, action, tone, onConfirm, onClose, busy, error, children }: {
  title: string; word: string; action: string; tone: 'bad' | 'warn'
  onConfirm: () => void; onClose: () => void; busy?: boolean; error?: string | null; children: ReactNode
}) {
  const [typed, setTyped] = useState('')
  const input = useRef<HTMLInputElement>(null)
  const titleId = useId()
  const ok = typed.trim() === word

  useEffect(() => {
    input.current?.focus()
    const esc = (e: KeyboardEvent) => e.key === 'Escape' && onClose()
    window.addEventListener('keydown', esc)
    return () => window.removeEventListener('keydown', esc)
  }, [onClose])

  const btn = tone === 'bad' ? 'bg-[#5C1E24] text-[#FFE1E1] border-[#8A3038]' : 'bg-[#4A3115] text-[#FFE3C2] border-[#7A5226]'
  return (
    <div className="fixed inset-0 z-50 flex flex-col justify-end bg-[rgba(5,7,10,0.72)] md:items-center md:justify-center" onClick={onClose}>
      <div role="dialog" aria-modal="true" aria-labelledby={titleId} onClick={e => e.stopPropagation()}
        className="flex flex-col gap-3.5 rounded-t-2xl border-t border-edge bg-panel px-[18px] pt-5 pb-[max(30px,env(safe-area-inset-bottom))] md:w-[460px] md:rounded-2xl md:border md:pb-6">
        <div aria-hidden className="h-1 w-10 self-center rounded bg-edge md:hidden" />
        <h2 id={titleId} className="m-0 text-lg font-semibold">{title}</h2>
        <div className="text-sm leading-normal text-dim">{children}</div>
        <form className="contents" onSubmit={e => { e.preventDefault(); if (ok && !busy) onConfirm() }}>
          <label className="flex flex-col gap-1.5 text-[13px] text-muted">
            <span>Type <span className="num text-text">{word}</span> to confirm</span>
            <input ref={input} value={typed} onChange={e => setTyped(e.target.value)} autoComplete="off" autoCapitalize="off" spellCheck={false}
              placeholder={word} className="min-h-12 rounded-[10px] border border-edge bg-bg px-3 text-base text-text" />
          </label>
          {error && <p className="m-0 text-sm text-bad">{error}</p>}
          <div className="grid grid-cols-2 gap-2.5">
            <button type="button" onClick={onClose} className="min-h-12 cursor-pointer rounded-[10px] border border-line bg-raised text-[15px] font-semibold text-text">Cancel</button>
            <button type="submit" disabled={!ok || busy} className={`min-h-12 rounded-[10px] border text-[15px] font-semibold disabled:cursor-not-allowed disabled:opacity-40 ${btn} ${ok && !busy ? 'cursor-pointer' : ''}`}>
              {busy ? 'Working…' : action}
            </button>
          </div>
        </form>
      </div>
    </div>
  )
}
