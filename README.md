# FunTrade - High-Performance Algorithmic Trading Bot

FunTrade is a Proof-of-Concept (PoC) event-driven algorithmic trading bot written in **C++20**. It is designed with a strict focus on low-latency execution, robust concurrency, and adherence to SOLID principles. The architecture decouples market data ingestion, strategy logic, and order execution, allowing researchers to seamlessly transition from historical backtesting to live paper trading without modifying core strategy code.

## Core Architecture

The system is built around a central Publisher/Subscriber messaging system to prevent blocking calls between the network and the trading logic.

*   **`DomainModels.hpp`**: Contains the foundational plain data structures (`MarketTick`, `OrderRequest`, `Side`, `OrderType`). Market ticks support both Level 1 (Trade) and Level 2 (Order Book Bid/Ask) data.
*   **`IEventBus`**: The central communication hub. It utilizes a **lock-free concurrent queue** (`moodycamel::ConcurrentQueue`) and a dedicated background dispatcher thread. Data feeds publish ticks instantly without blocking, and strategies receive them asynchronously.
*   **`IDataFeed`**: The interface for market data ingestion.
    *   `CsvHistoricalDataFeed`: Reads massive CSV files of past tick/order book data and pumps them into the event bus. Supports "As Fast As Possible" and "Real-Time Simulation" playback modes for backtesting.
    *   `AlpacaDataFeed`: Pre-configured to handle live WebSocket streams from the Alpaca exchange, parsing JSON arrays into domain models.
*   **`IExecutionEngine`**: The interface for routing orders.
    *   `DummyExecution`: Mocks order placements for backtesting.
    *   `AlpacaExecutionEngine`: Pre-configured to format domain `OrderRequest` structs into the JSON schemas expected by the Alpaca REST API.
*   **`IStrategy`**: The "brain" of the bot. Strategies subscribe to the Event Bus and evaluate incoming ticks.
    *   `OBIMomentum`: The primary PoC strategy. It utilizes the **Eigen3** library to maintain an optimized rolling window of Order Book Imbalance (OBI) calculations. It fires Buy/Sell signals when the Simple Moving Average (SMA) of the imbalance crosses specific thresholds.

## Tech Stack & Tooling

*   **Language:** C++20
*   **Build System:** CMake (Pitchfork project layout)
*   **Concurrency:** `std::thread`, `std::atomic`, `std::mutex`, and `moodycamel::ConcurrentQueue`.
*   **Mathematics:** `Eigen3` (Highly optimized, SIMD-vectorized C++ template library for linear algebra).
*   **Data Parsing:** `nlohmann/json` (Modern C++ JSON library).
*   **Testing:** Google Test (GTest) integrated via CTest.

## Building and Running

The project utilizes CMake's `FetchContent` to automatically download and link dependencies (GTest, Eigen, ConcurrentQueue, JSON).

```bash
mkdir -p build && cd build
cmake ..
make

# Run the unit tests
ctest --output-on-failure

# Run the backtester
./FunTrade
```

## Current Project Status

The project has successfully completed the implementation of the core architecture, the lock-free event pipeline, the backtesting infrastructure, and the Eigen-powered mathematical strategy logic. The current entry point (`main.cpp`) acts as a backtester, replaying a sample Level 2 data CSV file through the `OBIMomentum` strategy.

The next planned phase is the integration of live networking (e.g., using `Boost.Asio` or `Boost.Beast`) to connect the pre-mapped Alpaca data and execution classes to the live internet.