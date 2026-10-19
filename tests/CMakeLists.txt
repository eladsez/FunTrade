# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# We want to use GTest macro to discover tests
include(GoogleTest)

# Offline unit tests: no network, no broker, deterministic. Run on every build.
add_executable(unit_tests
    test_main.cpp
    test_event_bus.cpp
    test_basket_state.cpp
    test_basket_sizer.cpp
    test_execution_cycle.cpp
    test_portfolio_manager.cpp
    test_order_manager.cpp
    test_basket_strategy.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(unit_tests FunTradeLib GTest::gtest pthread)
gtest_discover_tests(unit_tests)

# Live broker integration tests place real (paper) orders and need a running gateway or
# API keys, so they are opt-in: cmake -DFUNTRADE_LIVE_TESTS=ON
option(FUNTRADE_LIVE_TESTS "Build broker integration tests (need IB Gateway / Alpaca keys)" OFF)
if(FUNTRADE_LIVE_TESTS)
    add_executable(test_ibkr_execution test_ibkr_execution.cpp)
    target_link_libraries(test_ibkr_execution FunTradeLib GTest::gtest_main)

    add_executable(test_alpaca_execution test_alpaca_execution.cpp)
    target_link_libraries(test_alpaca_execution FunTradeLib GTest::gtest_main)
endif()
