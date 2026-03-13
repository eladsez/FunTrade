# FunTrade - High-Performance Algorithmic Trading Bot

FunTrade is a event-driven algorithmic trading bot written in **C++20**. It is designed with a strict focus on low-latency execution, robust concurrency, and adherence to SOLID principles. The architecture decouples market data ingestion, strategy logic, and order execution, allowing researchers to seamlessly transition from historical backtesting to live paper trading without modifying core strategy code.

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

The project utilizes CMake's `FetchContent` to automatically download and link dependencies (GTest, Eigen, ConcurrentQueue, JSON, Boost, spdlog).  

```bash
# Build the project
bash scripts/build.sh

# Run the LIVE Paper Trading Bot
source .env
./build/FunTrade
```
