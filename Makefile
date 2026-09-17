CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude
BUILD := build
COMMON := src/order_book.cpp src/csv_reader.cpp
HEADERS := $(wildcard include/*.hpp)
TEST_HEADERS := tests/reference_book.hpp

.PHONY: all test benchmark clean

all: $(BUILD)/order_book_replay $(BUILD)/order_book_benchmark $(BUILD)/order_book_telemetry $(BUILD)/order_book_property_fuzz

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/order_book_replay: $(COMMON) src/main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(BUILD)/order_book_tests: $(COMMON) tests/order_book_tests.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(BUILD)/order_book_benchmark: src/order_book.cpp benchmarks/replay_benchmark.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(BUILD)/engine_contract_tests: $(COMMON) tests/engine_contract_tests.cpp $(HEADERS) $(TEST_HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(BUILD)/order_book_property_fuzz: src/order_book.cpp tests/order_book_property_fuzz.cpp $(HEADERS) $(TEST_HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

$(BUILD)/order_book_telemetry: src/order_book.cpp benchmarks/performance_telemetry.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

test: $(BUILD)/order_book_tests $(BUILD)/engine_contract_tests $(BUILD)/order_book_property_fuzz
	./$(BUILD)/order_book_tests
	./$(BUILD)/engine_contract_tests
	./$(BUILD)/order_book_property_fuzz 1 10000 $(BUILD)/fuzz_failure_seed_1.txt
	./$(BUILD)/order_book_property_fuzz 42 10000 $(BUILD)/fuzz_failure_seed_42.txt
	python3 -m unittest discover -s tests -p 'test_*.py'

benchmark: $(BUILD)/order_book_benchmark
	./$(BUILD)/order_book_benchmark 500000

clean:
	rm -f $(BUILD)/order_book_replay $(BUILD)/order_book_tests $(BUILD)/order_book_benchmark $(BUILD)/engine_contract_tests $(BUILD)/order_book_property_fuzz $(BUILD)/order_book_telemetry
