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

# Add EventBus tests
add_executable(test_event_bus test_event_bus.cpp)
target_link_libraries(test_event_bus FunTradeLib GTest::gtest_main pthread)
gtest_discover_tests(test_event_bus)

# Add Data Feed tests
add_executable(test_alpaca_data_feed test_alpaca_data_feed.cpp)
target_link_libraries(test_alpaca_data_feed FunTradeLib GTest::gtest_main)
gtest_discover_tests(test_alpaca_data_feed)

# Add Execution tests
add_executable(test_alpaca_execution test_alpaca_execution.cpp)
target_link_libraries(test_alpaca_execution FunTradeLib GTest::gtest_main)
gtest_discover_tests(test_alpaca_execution)

# Add PortfolioManager tests
add_executable(test_portfolio_manager test_portfolio_manager.cpp)
target_link_libraries(test_portfolio_manager FunTradeLib GTest::gtest_main)
gtest_discover_tests(test_portfolio_manager)

# Add OrderManager tests
add_executable(test_order_manager test_order_manager.cpp)
target_link_libraries(test_order_manager FunTradeLib GTest::gtest_main)
gtest_discover_tests(test_order_manager)

# Add System Infra Integration tests
add_executable(test_system_infra test_system_infra.cpp)
target_link_libraries(test_system_infra FunTradeLib GTest::gtest_main)
gtest_discover_tests(test_system_infra)
