# Algorithmic Trading Bot - Project Context & State

## 1. Project Overview
High-performance, event-driven C++20 Algorithmic Trading Bot optimized for low-latency execution on Linux.

## 2. Tech Stack & Architecture
* **Core:** C++20, Linux, CMake (Pitchfork layout).
* **Libraries:** Boost.Asio/Beast (Async I/O), nlohmann/json, Eigen3 (Math), spdlog, moodycamel::ConcurrentQueue (Lock-free).
* **Interfaces:** 
    * `IEventBus`: Central dispatcher using `std::variant<MarketTick, ExecutionReport>`.
    * `IDataFeed`: WebSocket-based market data normalization.
    * `IExecutionEngine`: REST/WebSocket order routing and status reporting.
    * `IStrategy`: Logic engine (currently OBI Momentum).
    * `PortfolioManager`: Real-time PnL and position state tracking.

## 3. Current Codebase State
* **Event System:** Thread-safe, non-blocking `EventBus` with subscriber filtering by symbol and event type.
* **Execution:** `AlpacaExecutionEngine` supports `MARKET`, `LIMIT`, and `TRAILING_STOP`. Simulated fills implemented for testing.
* **Risk/Portfolio:** `PortfolioManager` tracks realized/unrealized PnL, position flipping, and weighted average entry prices.
* **Strategy:** `OBIMomentum` uses `Eigen` for rolling window SMA calculations and `LIMIT` orders for aggressive yet protected entry/exit.
* **Testing:** 100% pass rate on unit tests for EventBus, DataFeed, Execution, and Portfolio modules with mocked network isolation.

## 4. Immediate Next Steps
* **Goal1:** Continue implementing PAPERTRADING tests for a full coverage of AlpacaExecutionEngine and PortfolioManager, Also ExecutionReport full coverage testing


* **Goal 2:** Add an `OrderManager` component to reconcile local order state with exchange state during initialization.
