#include "order_book.hpp"

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::vector<lob::Event> make_events(std::size_t pairs, const std::string& workload) {
    std::vector<lob::Event> events;
    events.reserve(pairs * 2);

    for (std::size_t i = 1; i <= pairs; ++i) {
        if (workload == "lifecycle") {
            const auto side = i % 2 == 0 ? lob::Side::Buy : lob::Side::Sell;
            const lob::Price price = side == lob::Side::Buy
                ? 10000 - static_cast<lob::Price>(i % 20)
                : 10001 + static_cast<lob::Price>(i % 20);
            const lob::Order order{i, side, price, 1 + i % 100};
            events.push_back({i * 2, lob::EventType::Add, order});
            events.push_back({i * 2 + 1, lob::EventType::Cancel, order});
        } else {
            const lob::Quantity quantity = 1 + i % 100;
            events.push_back({i * 2, lob::EventType::Add,
                              {i * 2 - 1, lob::Side::Sell, 10001, quantity}});
            events.push_back({i * 2 + 1, lob::EventType::Add,
                              {i * 2, lob::Side::Buy, 10001, quantity}});
        }
    }
    return events;
}

bool replay(const std::vector<lob::Event>& events, lob::OrderBook& book) {
    for (const auto& event : events) {
        const auto result = book.process(event);
        if (!result.accepted) {
            std::cerr << "unexpected rejected event: " << result.message << '\n';
            return false;
        }
    }
    return true;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t pairs = argc > 1 ? std::stoull(argv[1]) : 500000;
    const std::string workload = argc > 2 ? argv[2] : "lifecycle";
    if (pairs == 0 || (workload != "lifecycle" && workload != "matching")) {
        std::cerr << "usage: order_book_benchmark [pairs>0] [lifecycle|matching]\n";
        return EXIT_FAILURE;
    }
    const auto events = make_events(pairs, workload);

    // The throughput pass avoids a clock read around every event.
    lob::OrderBook throughput_book;
    const auto throughput_start = Clock::now();
    if (!replay(events, throughput_book)) return EXIT_FAILURE;
    const double seconds = std::chrono::duration<double>(
        Clock::now() - throughput_start).count();

    // A fresh book makes the instrumented latency pass independent and reproducible.
    lob::OrderBook latency_book;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(events.size());
    for (const auto& event : events) {
        const auto event_start = Clock::now();
        const auto result = latency_book.process(event);
        const auto event_end = Clock::now();
        if (!result.accepted) {
            std::cerr << "unexpected rejected event: " << result.message << '\n';
            return EXIT_FAILURE;
        }
        latencies_ns.push_back(std::chrono::duration<double, std::nano>(
            event_end - event_start).count());
    }
    std::sort(latencies_ns.begin(), latencies_ns.end());

    std::cout << std::fixed << std::setprecision(2)
              << "workload=" << workload << '\n'
              << "events=" << events.size() << '\n'
              << "elapsed_seconds=" << seconds << '\n'
              << "events_per_second=" << events.size() / seconds << '\n'
              << "latency_p50_ns=" << percentile(latencies_ns, 0.50) << '\n'
              << "latency_p95_ns=" << percentile(latencies_ns, 0.95) << '\n'
              << "latency_p99_ns=" << percentile(latencies_ns, 0.99) << '\n'
              << "latency_max_ns=" << latencies_ns.back() << '\n'
              << "active_orders=" << throughput_book.order_count() << '\n';
}
