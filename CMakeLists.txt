cmake_minimum_required(VERSION 3.14)
project(FunTrade VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compiler warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Fetch nlohmann/json automatically
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
)
FetchContent_MakeAvailable(nlohmann_json)

# Fetch moodycamel/concurrentqueue for lock-free async event bus
FetchContent_Declare(
    concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG v1.0.4
)
FetchContent_MakeAvailable(concurrentqueue)

# Fetch Eigen3 for high-performance matrix/vector math
FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG 3.4.0
)
FetchContent_MakeAvailable(eigen)

# Fetch spdlog for high-performance logging
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
)
FetchContent_MakeAvailable(spdlog)

# Locate source files excluding main.cpp for the library
file(GLOB_RECURSE LIB_SRC_FILES 
    "src/core/*.cpp"
    "src/data/*.cpp"
    "src/execution/*.cpp"
    "src/strategy/*.cpp"
    "src/risk/*.cpp"
)

# Find Boost and OpenSSL
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)

# Create a static library for the core logic
add_library(FunTradeLib STATIC ${LIB_SRC_FILES})
target_include_directories(FunTradeLib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(FunTradeLib PUBLIC 
    nlohmann_json::nlohmann_json 
    concurrentqueue 
    Eigen3::Eigen 
    spdlog::spdlog
    pthread
    Boost::system
    Boost::thread
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Create the main executable
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE FunTradeLib)

# Enable testing and add the tests directory
enable_testing()

option(ENABLE_PAPERTRADING_TEST "Enable live paper trading integration tests" OFF)
if(ENABLE_PAPERTRADING_TEST)
    add_definitions(-DPAPERTRADING)
    message(STATUS "Paper trading integration tests ENABLED")
endif()

add_subdirectory(tests)