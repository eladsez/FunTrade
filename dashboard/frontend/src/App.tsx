import { NavLink, Route, Routes } from 'react-router-dom'
import { LayoutGrid, List, Activity, Power, TrendingUp } from 'lucide-react'
import { useOverview } from './lib/api'
import { useStream } from './lib/live'
import { session, useNow } from './lib/time'
import { Dot } from './components/ui'
import Overview from './pages/Overview'
import Baskets from './pages/Baskets'
import Basket from './pages/Basket'
import Orders from './pages/Orders'
import System from './pages/System'

const NAV = [
  { to: '/', label: 'Overview', icon: LayoutGrid, end: true },
  { to: '/baskets', label: 'Baskets', icon: Activity },
  { to: '/orders', label: 'Orders', icon: List },
  { to: '/system', label: 'System', icon: Power },
]

function Header() {
  useNow(30_000)
  const s = session()
  const { connected } = useStream()
  const { data } = useOverview()
  const gw = data?.broker.bot_connected
  const botOk = data?.bot.state === 'running'
  const tone = !connected || !data ? 'off' : !botOk ? 'bad' : gw === false ? 'warn' : 'ok'
  const label = !connected ? 'Dashboard offline' : !data ? '…' : !botOk ? `Bot ${data.bot.state}` : gw === false ? 'Gateway down' : 'Bot running'
  return (
    <header className="sticky top-0 z-40 flex flex-wrap items-center gap-x-6 gap-y-3 border-b border-line bg-bar px-4 pt-[max(14px,env(safe-area-inset-top))] pb-3.5 md:px-8">
      <div className="flex items-center gap-2.5">
        <TrendingUp size={22} className="text-accent" aria-hidden />
        <span className="text-[17px] font-bold tracking-tight">FunTrade</span>
        <span className="rounded border border-short-bd px-1.5 py-0.5 text-[11px] font-semibold tracking-[0.06em] text-warn">PAPER</span>
      </div>
      <nav aria-label="Main" className="hidden gap-1 md:flex">
        {NAV.map(n => (
          <NavLink key={n.to} to={n.to} end={n.end}
            className={({ isActive }) => `rounded-md px-3.5 py-2 text-sm ${isActive ? 'bg-sel font-medium text-text' : 'text-muted hover:text-text'}`}>
            {n.label}
          </NavLink>
        ))}
      </nav>
      <div className="ml-auto flex items-center gap-4 text-[13px] text-muted">
        <span className="num hidden md:inline">NY {s.nyClock} · IL {s.ilClock}</span>
        <span className="num md:hidden">{s.ilClock} IL</span>
        <span className="flex items-center gap-1.5"><Dot tone={tone} /><span className="hidden sm:inline">{label}</span></span>
      </div>
    </header>
  )
}

function BottomNav({ halted }: { halted: boolean }) {
  return (
    <nav aria-label="Main" className="fixed inset-x-0 bottom-0 z-40 grid grid-cols-4 border-t border-line bg-bar pt-1.5 pb-[max(18px,env(safe-area-inset-bottom))] md:hidden">
      {NAV.map(({ to, label, icon: Icon, end }) => (
        <NavLink key={to} to={to} end={end}
          className={({ isActive }) => `relative flex min-h-12 flex-col items-center justify-center gap-[3px] text-[11px] ${isActive ? 'text-text' : 'text-muted'}`}>
          <Icon size={22} aria-hidden />{label}
          {to === '/system' && halted && <span aria-label="needs attention" className="absolute top-1.5 right-[30%] size-2 rounded-full bg-alert" />}
        </NavLink>
      ))}
    </nav>
  )
}

export default function App() {
  const { data } = useOverview()
  return (
    <div className="min-h-dvh pb-24 md:pb-0">
      <Header />
      <main className="mx-auto flex max-w-[1376px] flex-col gap-4 px-4 pt-3.5 pb-12 md:gap-5 md:px-8 md:pt-6">
        <Routes>
          <Route path="/" element={<Overview />} />
          <Route path="/baskets" element={<Baskets />} />
          <Route path="/baskets/:id" element={<Basket />} />
          <Route path="/orders" element={<Orders />} />
          <Route path="/system" element={<System />} />
          <Route path="*" element={<p className="text-muted">Not found.</p>} />
        </Routes>
      </main>
      <BottomNav halted={!!data?.halted.length} />
    </div>
  )
}
