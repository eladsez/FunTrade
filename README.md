# FunTrade - High-Performance Algorithmic Trading Bot

FunTrade is a event-driven algorithmic trading bot written in **C++20**. It is designed with a strict focus on low-latency execution, robust concurrency, and adherence to SOLID principles. The architecture decouples market data ingestion, strategy logic, and order execution, allowing researchers to seamlessly transition from historical backtesting to live paper trading without modifying core strategy code.

## Strategy

Daily basket mean reversion on cointegrated 3-stock baskets (`config/basket_configs/`, produced
by `scripts/compute.py`). At each close the bot computes the basket's z-score from
dividend-adjusted daily closes; at the next session it acts with market-on-close orders, so it
fills at the official close exactly as the research simulator assumes
(`scripts/parity_check.py` verifies trade-for-trade parity). State is persisted in `state/`,
reconciled against the broker on every start, and guarded by portfolio risk limits
(`config/risk.json`). Operating instructions: `docs/OPERATIONS.md`. Status and history of the
hardening work: `docs/ROADMAP.md`.

## Core Architecture

The system is built around a central Publisher/Subscriber messaging system to prevent blocking calls between the network and the trading logic.

*   **`DomainModels.hpp`**: Contains the foundational plain data structures (`MarketTick`, `OrderRequest`, `Side`, `OrderType`). Market ticks support both Level 1 (Trade) and Level 2 (Order Book Bid/Ask/Quote) data.
*   **`IEventBus`**: The central communication hub. It utilizes a **lock-free concurrent queue** (`moodycamel::ConcurrentQueue`) and a dedicated background dispatcher thread. Data feeds publish ticks instantly without blocking, and strategies receive them asynchronously.
*   **`IDataFeed`**: The interface for market data ingestion.
    *   `AlpacaRtDataFeed`: Connects to live Alpaca WebSocket streams (`Boost.Beast`) for 24/7 Crypto market data, parsing JSON arrays into domain models in real-time.
*   **`IExecutionEngine`**: The interface for routing orders.
    *   `AlpacaExecutionEngine`: A high-performance REST client (`Boost.Beast`) that formats domain `OrderRequest` structs into JSON schemas for the Alpaca Paper Trading API, and utilizes a dedicated WebSocket `trade_updates` stream for non-blocking asynchronous execution.
*   **`PortfolioManager`**: Handles real-time PnL tracking and position reconciliation.
*   **`OrderManager`**: Tracks order states and handles reconciliation via `client_order_id`.
*   **`IStrategy`**: The "brain" of the bot. Strategies subscribe to the Event Bus and evaluate incoming market events.
*   **`Logger`**: Thread-safe, colored, microsecond-precision console logging powered by `spdlog`.

## Tech Stack & Tooling

*   **Language:** C++20
*   **Build System:** CMake (Pitchfork project layout)
*   **Concurrency:** `std::thread`, `std::atomic`, `std::mutex`, and `moodycamel::ConcurrentQueue`.
*   **Networking:** `Boost.Asio`, `Boost.Beast`, and `OpenSSL` for blazing-fast WebSocket and REST connectivity.
*   **Data Parsing/Logging:** `nlohmann/json` (Modern C++ JSON) and `spdlog`.
*   **Testing:** Google Test, with a synchronous event bus and a scriptable execution engine (`tests/fakes/`) for deterministic offline tests.

## Security (Live Trading)

**DO NOT hardcode your API keys.** The live bot expects your Alpaca credentials to be securely provided via environment variables. Create a `.env` file in the root directory (which is ignored by Git) to safely store your keys:

```bash
# .env
export APCA_API_KEY_ID="your_real_paper_key_here"
export APCA_API_SECRET_KEY="your_real_paper_secret_here"
```

## Building and Running

CMake's `FetchContent` downloads GTest, ConcurrentQueue, nlohmann/json and spdlog.
These must be installed on the system:

* **Boost** >= 1.75 (`system`, `thread`, plus header-only Asio/Beast)
* **OpenSSL**
* **Protobuf** (required by the `external/twsapi` submodule)

On Ubuntu: `sudo apt install libboost-system-dev libboost-thread-dev libssl-dev libprotobuf-dev protobuf-compiler`.
Fetch the TWS API submodule first: `git submodule update --init`.

```bash
# Build the project
bash scripts/build.sh

# Run the offline unit tests (no network or broker needed)
cd build && ctest

# Broker integration tests place real paper orders and are opt-in:
#   cmake -DFUNTRADE_LIVE_TESTS=ON .. && make && ./tests/test_ibkr_execution

# Run the bot against IB Gateway (paper, 127.0.0.1:4002)
./build/FunTrade
```

Tests named `DISABLED_PhaseN_*` describe known bugs; each is enabled when the
corresponding phase in `docs/ROADMAP.md` fixes it.
