cmake_minimum_required(VERSION 3.16)
project(order_book_replay VERSION 0.7.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(order_book src/order_book.cpp src/csv_reader.cpp)
target_include_directories(order_book PUBLIC include)
target_compile_options(order_book PRIVATE -Wall -Wextra -Wpedantic)

add_executable(order_book_replay src/main.cpp)
target_link_libraries(order_book_replay PRIVATE order_book)

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE order_book)

add_executable(engine_contract_tests tests/engine_contract_tests.cpp)
target_link_libraries(engine_contract_tests PRIVATE order_book)

add_executable(order_book_benchmark benchmarks/replay_benchmark.cpp)
target_link_libraries(order_book_benchmark PRIVATE order_book)

add_executable(order_book_telemetry benchmarks/performance_telemetry.cpp)
target_link_libraries(order_book_telemetry PRIVATE order_book)

add_executable(order_book_property_fuzz tests/order_book_property_fuzz.cpp)
target_link_libraries(order_book_property_fuzz PRIVATE order_book)

add_executable(order_book_differential tests/order_book_differential.cpp)
target_include_directories(order_book_differential PRIVATE tests)
target_link_libraries(order_book_differential PRIVATE order_book)

enable_testing()
add_test(NAME order_book_tests COMMAND order_book_tests)
add_test(NAME engine_contract_tests COMMAND engine_contract_tests)
add_test(NAME replay_marketable_modify
    COMMAND order_book_replay ${CMAKE_CURRENT_SOURCE_DIR}/data/marketable_modify.csv)
set_tests_properties(replay_marketable_modify PROPERTIES
    PASS_REGULAR_EXPRESSION "events=5 rejected=0 trades=2 traded_quantity=7 active_orders=0")
add_test(NAME benchmark_lifecycle_smoke COMMAND order_book_benchmark 100 lifecycle)
add_test(NAME benchmark_matching_smoke COMMAND order_book_benchmark 100 matching)
add_test(NAME telemetry_smoke COMMAND order_book_telemetry telemetry_smoke.csv 4 100 2 50)
add_test(NAME property_fuzz_seed_1 COMMAND order_book_property_fuzz 1 2000 fuzz_failure_seed_1.txt)
add_test(NAME property_fuzz_seed_42 COMMAND order_book_property_fuzz 42 2000 fuzz_failure_seed_42.txt)
add_test(NAME differential_seed_1 COMMAND order_book_differential 1 5000 differential_failure_seed_1.csv)
add_test(NAME differential_seed_42 COMMAND order_book_differential 42 5000 differential_failure_seed_42.csv)
add_test(NAME differential_seed_2026 COMMAND order_book_differential 2026 5000 differential_failure_seed_2026.csv)

# Regression corpus: every saved reproducer must replay identically forever.
file(GLOB DIFFERENTIAL_CORPUS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/corpus/*.csv)
foreach(corpus_file ${DIFFERENTIAL_CORPUS})
    get_filename_component(corpus_name ${corpus_file} NAME_WE)
    add_test(NAME corpus_${corpus_name} COMMAND order_book_differential --replay ${corpus_file})
endforeach()
