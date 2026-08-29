CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude
BUILD := build
COMMON := src/order_book.cpp src/csv_reader.cpp

.PHONY: all test benchmark clean

all: $(BUILD)/order_book_replay $(BUILD)/order_book_benchmark

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/order_book_replay: $(COMMON) src/main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/order_book_tests: $(COMMON) tests/order_book_tests.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/order_book_benchmark: src/order_book.cpp benchmarks/replay_benchmark.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/order_book_tests
	./$(BUILD)/order_book_tests

benchmark: $(BUILD)/order_book_benchmark
	./$(BUILD)/order_book_benchmark 500000

clean:
	rm -f $(BUILD)/order_book_replay $(BUILD)/order_book_tests $(BUILD)/order_book_benchmark
