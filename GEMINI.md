# Algorithmic Trading Bot - Project Context & State

## 1. Project Overview
This project is a high-performance, event-driven Algorithmic Trading Bot Proof of Concept (PoC) written in **C++20**. The architecture strictly follows **SOLID principles** (specifically Dependency Inversion and Open/Closed), designed to decouple market data ingestion, strategy logic, and order execution.

### Tech Stack & Tooling:
* **Language:** C++20
* **Build System:** CMake (Pitchfork project layout)
* **OS Target:** Linux (optimized for low-latency execution)
* **Core Libraries:** `Boost.Asio` / `Boost.Beast` (Async Networking/WebSockets), `nlohmann/json` (API parsing), `Eigen3` (Math), `spdlog` (High-performance logging).

## 2. Core Architecture (The Interfaces)
The system is built around a central Publisher/Subscriber messaging system to prevent blocking calls between the network and the trading logic.

* `DomainModels.hpp`: Contains plain data structures (`MarketTick`, `OrderRequest`, `Side`, `OrderType`).
* `IEventBus`: The central communication hub. Data feeds publish ticks here, and strategies subscribe to specific symbols.
* `IDataFeed`: Interface for connecting to exchanges (e.g., WebSocket) and normalizing incoming data into `MarketTick` structs.
* `IExecutionEngine`: Interface for authenticating and routing `OrderRequest` structs to the broker (REST API/FIX).
* `IStrategy`: The trading logic brain. Evaluates ticks and triggers executions.

## 3. Current State
* **Scaffolding:** The directory structure (`include/`, `src/`, `config/`, `tests/`) and `CMakeLists.txt` are established.
* **Concurrency:** The system utilizes a lock-free `moodycamel::ConcurrentQueue` for blazing-fast event dispatching.
* **Logging:** Fully integrated `spdlog` for thread-safe, colored, microsecond-precision console logging.
* **Live Connectivity:** `Boost.Beast` has been fully implemented for both REST (`AlpacaExecutionEngine`) and WebSockets (`AlpacaDataFeed`), enabling secure, 24/7 live paper trading (currently defaulting to Crypto `BTC/USD`).
* **Execution Tested:** Fractional balances, strict string precision formatting, and HTTP 204 cancellations have been tested live against Alpaca.
* **Testing:** Comprehensive Google Test framework with 100% mocked isolation for network dependencies, plus a dedicated live paper-trading integration test guarded by CMake flags.

## 4. Trading Strategies Explored
We are focusing on strategies suitable for low-latency environments:
1.  **Order Book Imbalance (OBI):** Analyzing Level 2 market data to calculate the ratio of bid/ask volume, predicting micro-momentum breakouts. (Currently implemented PoC).
2.  **Statistical Arbitrage (Pairs Trading):** Using cointegration and Z-Scores to trade mean-reverting spreads between correlated assets. 

## 5. Development Roadmap

### [DONE] Phase 1: Code Separation & Structuring
* Extracted the mock classes from `main.cpp` into respective `.cpp` files.

### [DONE] Phase 2: Asynchronous & Lock-Free Data Flow
* Upgraded `EventBus` to use a lock-free concurrent queue and a background dispatcher thread.

### [DONE] Phase 3: Historical Data Integration (Backtesting Prep)
* Implemented a `CsvHistoricalDataFeed` for "As Fast As Possible" or real-time CSV playback.

### [DONE] Phase 4: Mathematical Logic
* Integrated the `Eigen` library for high-performance matrix/vector math. Implemented the Order Book Imbalance (OBI) rolling window calculation.

### [DONE] Phase 5: Live API Integration (Paper Trading)
* Implemented live WebSockets (`wss://stream.data.alpaca.markets`) for real-time Crypto quotes and order book snapshots.
* Implemented synchronous REST HTTPS client using Boost.Beast for order placement, balance checking, and cancellation.
* Isolated live tests using `#ifdef PAPERTRADING` and CMake flags to prevent accidental real-account modifications.

### [TODO] Phase 6: Risk Management & Advanced Strategy
* Build a `PortfolioManager` to track dynamic PnL, position sizing, and implement hard stop-losses.
* Upgrade the `OBIMomentum` strategy to utilize Limit orders or trailing stops instead of Market orders.
* Alternatively, implement the Statistical Arbitrage (Pairs Trading) strategy.

---
**Note for Next Gemini Session:**
Phase 5 is complete! The bot is structurally sound, rigorously tested, fully asynchronous, and connected to live markets via Boost.Beast. All raw `std::cout` statements have been replaced with `spdlog`. The next session should focus on Phase 6: Implementing Risk Management (Portfolio tracking, position sizing) or building out a more advanced algorithmic strategy.