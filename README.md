# FunTrade - High-Performance Algorithmic Trading Bot

FunTrade is a event-driven algorithmic trading bot written in **C++20**. It is designed with a strict focus on low-latency execution, robust concurrency, and adherence to SOLID principles. The architecture decouples market data ingestion, strategy logic, and order execution, allowing researchers to seamlessly transition from historical backtesting to live paper trading without modifying core strategy code.

## Core Architecture

The system is built around a central Publisher/Subscriber messaging system to prevent blocking calls between the network and the trading logic.

*   **`DomainModels.hpp`**: Contains the foundational plain data structures (`MarketTick`, `OrderRequest`, `Side`, `OrderType`). Market ticks support both Level 1 (Trade) and Level 2 (Order Book Bid/Ask/Quote) data.
*   **`IEventBus`**: The central communication hub. It utilizes a **lock-free concurrent queue** (`moodycamel::ConcurrentQueue`) and a dedicated background dispatcher thread. Data feeds publish ticks instantly without blocking, and strategies receive them asynchronously.
*   **`IDataFeed`**: The interface for market data ingestion.
    *   `CsvHistoricalDataFeed`: Reads massive CSV files of past tick/order book data and pumps them into the event bus. Supports "As Fast As Possible" and "Real-Time Simulation" playback modes for backtesting.
    *   `AlpacaDataFeed`: Connects to live Alpaca WebSocket streams (`Boost.Beast`) for 24/7 Crypto market data, parsing JSON arrays into domain models in real-time.
*   **`IExecutionEngine`**: The interface for routing orders.
    *   `DummyExecution`: Mocks order placements for backtesting.
    *   `AlpacaExecutionEngine`: A high-performance REST client (`Boost.Beast`) that formats domain `OrderRequest` structs into JSON schemas for the Alpaca Paper Trading API, and utilizes a dedicated WebSocket `trade_updates` stream for non-blocking asynchronous execution.
*   **`PortfolioManager`**: Handles real-time PnL tracking and position reconciliation.
*   **`OrderManager`**: Tracks order states and handles reconciliation via `client_order_id`.
*   **`IStrategy`**: The "brain" of the bot. Strategies subscribe to the Event Bus and evaluate incoming ticks.
    *   `OBIMomentum`: The primary strategy. It utilizes the **Eigen3** library to maintain an optimized rolling window of Order Book Imbalance (OBI) calculations. It fires Buy/Sell signals when the Simple Moving Average (SMA) of the imbalance crosses specific thresholds.
*   **`Logger`**: Thread-safe, colored, microsecond-precision console logging powered by `spdlog`.

## Tech Stack & Tooling

*   **Language:** C++20
*   **Build System:** CMake (Pitchfork project layout)
*   **Concurrency:** `std::thread`, `std::atomic`, `std::mutex`, and `moodycamel::ConcurrentQueue`.
*   **Networking:** `Boost.Asio`, `Boost.Beast`, and `OpenSSL` for blazing-fast WebSocket and REST connectivity.
*   **Mathematics:** `Eigen3` (Highly optimized, SIMD-vectorized C++ template library for linear algebra).
*   **Data Parsing/Logging:** `nlohmann/json` (Modern C++ JSON) and `spdlog`.
*   **Testing:** Google Test (GTest) with dedicated mock sub-classes to safely isolate network code.

## Security (Live Trading)

**DO NOT hardcode your API keys.** The live bot expects your Alpaca credentials to be securely provided via environment variables. Create a `.env` file in the root directory (which is ignored by Git) to safely store your keys:

```bash
# .env
export APCA_API_KEY_ID="your_real_paper_key_here"
export APCA_API_SECRET_KEY="your_real_paper_secret_here"
```

## Building and Running

The project utilizes CMake's `FetchContent` to automatically download and link dependencies (GTest, Eigen, ConcurrentQueue, JSON, Boost, spdlog). It is configured to build aggressively fast using `-j14`.

```bash
# Build the project
bash scripts/build.sh

# Run the unit tests (Network operations are safely mocked)
cd build && ctest --output-on-failure -j14 -R "^test_"

# Run the Offline Backtester (Uses sample CSV data)
./build/FunTrade

# Run the LIVE Paper Trading Bot
source .env
./build/FunTrade --live
```

### Running Live Integration Tests

If you wish to test the physical connection to Alpaca and verify order placement (Warning: This will place paper trades!), you must explicitly enable the CMake flag:

```bash
cmake -B build -DENABLE_PAPERTRADING_TEST=ON
cmake --build build -j14
source .env
./build/tests/test_alpaca_execution
```

## Current Project Status

The core infrastructure is 100% complete and fully verified. The bot successfully handles live WebSockets, lock-free dispatching, math calculations, and live REST order execution via `Boost.Beast`. 

Risk Management and Order Tracking have been implemented. `PortfolioManager` and `OrderManager` now support state reconciliation via `syncWithExchange()` to align local state with Alpaca on startup, and `placeOrder` is fully non-blocking with status updates published to the `EventBus`. 

The next phase will focus on refining the algorithmic strategies.