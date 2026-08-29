#include "order_book.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    const std::size_t pairs = argc > 1 ? std::stoull(argv[1]) : 500000;
    std::vector<lob::Event> events;
    events.reserve(pairs * 2);

    for (std::size_t i = 1; i <= pairs; ++i) {
        const auto side = i % 2 == 0 ? lob::Side::Buy : lob::Side::Sell;
        const lob::Price price = side == lob::Side::Buy
            ? 10000 - static_cast<lob::Price>(i % 20)
            : 10001 + static_cast<lob::Price>(i % 20);
        events.push_back({i * 2, lob::EventType::Add, {i, side, price, 1 + i % 100}});
        events.push_back({i * 2 + 1, lob::EventType::Cancel, {i, side, price, 1 + i % 100}});
    }

    lob::OrderBook book;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& event : events) {
        const auto result = book.process(event);
        if (!result.accepted) {
            std::cerr << "unexpected rejected event: " << result.message << '\n';
            return EXIT_FAILURE;
        }
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "events=" << events.size() << '\n'
              << "elapsed_seconds=" << seconds << '\n'
              << "events_per_second=" << events.size() / seconds << '\n'
              << "active_orders=" << book.order_count() << '\n';
}
