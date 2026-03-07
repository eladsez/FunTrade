# Algorithmic Trading Bot - Project Context & State

## 1. Project Overview
This project is a high-performance, event-driven Algorithmic Trading Bot Proof of Concept (PoC) written in **C++20**. The architecture strictly follows **SOLID principles** (specifically Dependency Inversion and Open/Closed), designed to decouple market data ingestion, strategy logic, and order execution.

### Tech Stack & Tooling:
* **Language:** C++20
* **Build System:** CMake (Pitchfork project layout)
* **OS Target:** Linux (optimized for low-latency execution)
* **Core Libraries:** `Boost.Asio` (planned for async networking), `nlohmann/json` (configuration/API parsing).

## 2. Core Architecture (The Interfaces)
The system is built around a central Publisher/Subscriber messaging system to prevent blocking calls between the network and the trading logic.

* `DomainModels.hpp`: Contains plain data structures (`MarketTick`, `OrderRequest`, `Side`, `OrderType`).
* `IEventBus`: The central communication hub. Data feeds publish ticks here, and strategies subscribe to specific symbols.
* `IDataFeed`: Interface for connecting to exchanges (e.g., WebSocket) and normalizing incoming data into `MarketTick` structs.
* `IExecutionEngine`: Interface for authenticating and routing `OrderRequest` structs to the broker (REST API/FIX).
* `IStrategy`: The trading logic brain. Evaluates ticks and triggers executions.

## 3. Current State
* **Scaffolding:** The directory structure (`include/`, `src/`, `config/`, `tests/`) and `CMakeLists.txt` have been successfully created.
* **PoC Implementation:** Extracted mock implementations into their own files. `main.cpp` wires `MockDataFeed`, `DummyExecution`, `OBIMomentum` strategy, and an asynchronous `EventBus`.
* **Demonstrated Flow:** The current code successfully demonstrates Dependency Injection, component wiring, and an asynchronous end-to-end data flow: Data Feed pushes a tick to a lock-free queue $\rightarrow$ Event Bus routes it on a background thread $\rightarrow$ Strategy evaluates price $\rightarrow$ Execution Engine fires a mock order.
* **Testing:** Google Test framework integrated with unit tests for `EventBus`, `AlpacaDataFeed`, and `AlpacaExecutionEngine`.

## 4. Trading Strategies Explored
We are focusing on strategies suitable for low-latency environments:
1.  **Order Book Imbalance (OBI):** Analyzing Level 2 market data to calculate the ratio of bid/ask volume, predicting micro-momentum breakouts.
2.  **Statistical Arbitrage (Pairs Trading):** Using cointegration and Z-Scores to trade mean-reverting spreads between correlated assets. 

## 5. Recommended Next Steps (Roadmap)

To evolve this from a single-file mock into a robust trading system, we should proceed in the following order:

### [DONE] Phase 1: Code Separation & Structuring
* Extracted the mock classes from `main.cpp` into respective `.cpp` files in the `src/` directory.

### [DONE] Phase 2: Asynchronous & Lock-Free Data Flow
* Upgraded `EventBus` to use a **lock-free concurrent queue** (`moodycamel::ConcurrentQueue`) and a background dispatcher thread.

### [DONE] Phase 3: Historical Data Integration (Backtesting Prep)
* Implemented a `CsvHistoricalDataFeed` that inherits from `IDataFeed`.
* It reads historical market ticks (including Level 2 Order Book data) from a CSV file and pumps them into the `EventBus`, allowing for real-time or as-fast-as-possible playback.

### [DONE] Phase 4: Mathematical Logic
* Integrated the `Eigen` library for high-performance matrix/vector math.
* Implemented the Order Book Imbalance (OBI) calculation logic using an `Eigen::VectorXd` rolling window, triggering mock executions when the Simple Moving Average crosses a momentum threshold.

### [IN PROGRESS] Phase 5: Live API Integration (Paper Trading)
* **[DONE]** Implemented `AlpacaDataFeed` and `AlpacaExecutionEngine` mapping logic utilizing `nlohmann/json`.
* **[TODO]** Implement actual networking using `Boost.Asio` or similar HTTP/WS clients for live connectivity.
