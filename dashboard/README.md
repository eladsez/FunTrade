# FunTrade dashboard

What the bot is doing, what the broker holds, and the two controls, on desktop and phone.
One container: FastAPI (`backend/`) serving the built React app (`frontend/`) and an API.

| Source | What it gives |
|---|---|
| `state/baskets/*.json`, `state/positions.json` | Basket states, books |
| `state/journal.jsonl` | History: close z and decision per day, cycles, fills, halts |
| `state/status.json` | Bot heartbeat, equity, exposure, limits |
| `logs/funtrade.log` | Warnings and errors |
| bot WebSocket `:9000` | Live prices, intraday z (display only) |
| IBKR, client id 17 | Broker's open orders, positions, executions (read-only) |

Writes: `state/KILL`, `state/FLATTEN`, and halt clears while the bot is stopped. See
`docs/OPERATIONS.md` → Dashboard for access and operations.

```bash
dashboard/dev.sh up       # http://localhost:5173 (hot reload), API on :8090, sample data
dashboard/dev.sh sample   # reset the sample data
dashboard/dev.sh test     # lint + typecheck + build + backend tests (= the image build)
dashboard/dev.sh image    # the production image on :8091
dashboard/dev.sh lock     # re-pin backend/requirements.txt (hashes) after editing requirements.in
```

Nothing runs on the host but Docker.
