cmake_minimum_required(VERSION 3.25)
project(valstream VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VALSTREAM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(VALSTREAM_BUILD_TESTS "Build unit and integration tests" ON)

if(VALSTREAM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Crow CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Everything except the HTTP server and main(), so tests link without Crow.
add_library(valstream_core STATIC
  src/act_resolver.cpp
  src/config.cpp
  src/cpr_transport.cpp
  src/diff_engine.cpp
  src/event_bus.cpp
  src/fixture_transport.cpp
  src/json.cpp
  src/poller.cpp
  src/rate_limiter.cpp
  src/riot_client.cpp
  src/snapshot_store.cpp
)
target_include_directories(valstream_core PUBLIC include)
target_compile_options(valstream_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
target_link_libraries(valstream_core PUBLIC
  cpr::cpr
  nlohmann_json::nlohmann_json
  unofficial::sqlite3::sqlite3
  spdlog::spdlog
  Threads::Threads
)

add_executable(valstream src/main.cpp src/server.cpp)
target_compile_options(valstream PRIVATE -Wall -Wextra)
target_link_libraries(valstream PRIVATE valstream_core Crow::Crow)

if(VALSTREAM_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  add_executable(valstream_tests
    tests/act_resolver_test.cpp
    tests/diff_engine_test.cpp
    tests/integration_test.cpp
    tests/rate_limiter_test.cpp
    tests/riot_client_test.cpp
    tests/snapshot_store_test.cpp
  )
  target_compile_definitions(valstream_tests PRIVATE VALSTREAM_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  target_link_libraries(valstream_tests PRIVATE valstream_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(valstream_tests DISCOVERY_TIMEOUT 30)
endif()
