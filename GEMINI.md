# Algorithmic Trading Bot - Project Context & State

## 1. Project Overview
High-performance, event-driven C++20 algorithmic trading bot optimized for low-latency execution on Linux via Alpaca.

## 2. AI Rules
* **Test Filtering:** Run only project tests using `ctest -R test_` to skip external dependency suites.
* **Verification:** Maintain a 100% pass rate. Every change to execution or risk modules requires a corresponding `PAPERTRADING` integration test.
* **Security:** Never log or commit `APCA_API_KEY_ID` or `APCA_API_SECRET_KEY`.

## 3. Tech Stack & Architecture
* **Core:** C++20, Linux, CMake, Boost.Asio/Beast, nlohmann/json, Eigen3, spdlog, moodycamel::ConcurrentQueue.
* **IEventBus:** Lock-free dispatcher for `MarketTick` and `ExecutionReport`.
* **IDataFeed:** WebSocket-based market data normalization (Crypto v1beta3).
* **IExecutionEngine:** Asynchronous REST order routing with dedicated WebSocket `trade_updates` stream.
* **PortfolioManager:** Real-time PnL tracking and position reconciliation.
* **OrderManager:** State tracking and reconciliation via `client_order_id`.

## 4. Current Codebase State
* **Asynchronous Execution:** `placeOrder` is non-blocking; status updates are handled via WS callbacks and published to the `EventBus`.
* **State Reconciliation:** `PortfolioManager` and `OrderManager` implement `syncWithExchange()` to align local state with Alpaca on startup.
* **Concurrency:** Multi-threaded architecture with dedicated IO contexts for REST, Market Data WS, and Trade Update WS.
* **Validation:** All unit and paper-trading integration tests for core infrastructure are passing.

## 5. Immediate Next Steps
* **Infra Coverage Goal:** Implement a comprehensive `SystemStressTest` that exercises the full non-strategy loop (AlpacaDataFeed -> EventBus -> OrderManager -> PortfolioManager) under simulated high-frequency burst conditions using a real Paper Trading connection to verify race-condition safety and state integrity.
