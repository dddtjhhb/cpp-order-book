#include "order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

lob::Event add_event(std::uint64_t timestamp, lob::OrderId id, lob::Side side,
                     lob::Price price, lob::Quantity quantity) {
    return {timestamp, lob::EventType::Add, {id, side, price, quantity}};
}

lob::Event cancel_event(std::uint64_t timestamp, lob::OrderId id, lob::Side side,
                        lob::Price price, lob::Quantity quantity) {
    return {timestamp, lob::EventType::Cancel, {id, side, price, quantity}};
}

}  // namespace

int main(int argc, char** argv) {
    const std::string output_path = argc > 1 ? argv[1] : "performance_telemetry.csv";
    const std::size_t batches = argc > 2 ? std::stoull(argv[2]) : 120;
    const std::size_t events_per_batch = argc > 3 ? std::stoull(argv[3]) : 20000;
    const std::size_t change_batch = argc > 4 ? std::stoull(argv[4]) : batches / 2;
    const std::size_t post_matching_percent = argc > 5 ? std::stoull(argv[5]) : 50;

    if (batches < 2 || events_per_batch < 2 || events_per_batch % 2 != 0 ||
        change_batch == 0 || change_batch >= batches || post_matching_percent > 100) {
        std::cerr << "usage: order_book_telemetry [output.csv] [batches>=2] "
                     "[even_events_per_batch>=2] [change_batch] "
                     "[post_matching_percent=0..100]\n";
        return EXIT_FAILURE;
    }

    std::ofstream output(output_path);
    if (!output) {
        std::cerr << "could not open telemetry output: " << output_path << '\n';
        return EXIT_FAILURE;
    }
    output << "batch,phase,matching_percent,events,elapsed_ns,throughput_eps,"
              "p50_ns,p95_ns,p99_ns,max_ns,rejected_events,active_orders,"
              "bid_levels,ask_levels\n";
    output << std::fixed << std::setprecision(2);

    lob::OrderBook book;
    lob::OrderId next_order_id = 1;
    std::uint64_t timestamp = 1;
    const std::size_t pairs_per_batch = events_per_batch / 2;
    const std::size_t warmup_pairs = pairs_per_batch * 10;

    // Warm the instruction/data paths before collecting the calibration prefix.
    for (std::size_t pair = 0; pair < warmup_pairs; ++pair) {
        const auto side = pair % 2 == 0 ? lob::Side::Buy : lob::Side::Sell;
        const lob::Price price = side == lob::Side::Buy
            ? 10000 - static_cast<lob::Price>(pair % 20)
            : 10001 + static_cast<lob::Price>(pair % 20);
        const lob::Quantity quantity = 1 + (pair % 100);
        const auto id = next_order_id++;
        const auto add = book.process(add_event(timestamp++, id, side, price, quantity));
        const auto cancel = book.process(cancel_event(timestamp++, id, side, price, quantity));
        if (!add.accepted || !cancel.accepted) {
            std::cerr << "warmup invariant failed\n";
            return EXIT_FAILURE;
        }
    }

    for (std::size_t batch = 0; batch < batches; ++batch) {
        const bool regression = batch >= change_batch;
        const std::size_t matching_percent = regression ? post_matching_percent : 0;
        std::vector<lob::Event> events;
        events.reserve(events_per_batch);
        std::vector<double> latencies_ns;
        latencies_ns.reserve(events_per_batch);
        std::size_t rejected_events = 0;

        for (std::size_t pair = 0; pair < pairs_per_batch; ++pair) {
            const bool matching_pair = pair % 100 < matching_percent;
            const lob::Quantity quantity = 1 + (pair % 100);

            if (matching_pair) {
                events.push_back(add_event(
                    timestamp++, next_order_id++, lob::Side::Sell, 10001, quantity));
                events.push_back(add_event(
                    timestamp++, next_order_id++, lob::Side::Buy, 10001, quantity));
            } else {
                const auto side = pair % 2 == 0 ? lob::Side::Buy : lob::Side::Sell;
                const lob::Price price = side == lob::Side::Buy
                    ? 10000 - static_cast<lob::Price>(pair % 20)
                    : 10001 + static_cast<lob::Price>(pair % 20);
                const auto id = next_order_id++;
                events.push_back(add_event(timestamp++, id, side, price, quantity));
                events.push_back(cancel_event(timestamp++, id, side, price, quantity));
            }
        }

        const auto batch_start = Clock::now();
        for (const auto& event : events) {
            const auto event_start = Clock::now();
            const auto result = book.process(event);
            const auto event_end = Clock::now();
            latencies_ns.push_back(std::chrono::duration<double, std::nano>(
                event_end - event_start).count());
            if (!result.accepted) ++rejected_events;
        }
        const auto batch_end = Clock::now();

        std::sort(latencies_ns.begin(), latencies_ns.end());
        const double elapsed_ns = std::chrono::duration<double, std::nano>(
            batch_end - batch_start).count();
        const double throughput = static_cast<double>(events_per_batch) * 1e9 / elapsed_ns;

        output << batch << ',' << (regression ? "regression" : "baseline") << ','
               << matching_percent << ',' << events_per_batch << ',' << elapsed_ns << ','
               << throughput << ',' << percentile(latencies_ns, 0.50) << ','
               << percentile(latencies_ns, 0.95) << ',' << percentile(latencies_ns, 0.99)
               << ',' << latencies_ns.back() << ',' << rejected_events << ','
               << book.order_count() << ',' << book.bid_level_count() << ','
               << book.ask_level_count() << '\n';

        if (rejected_events != 0 || book.order_count() != 0) {
            std::cerr << "telemetry invariant failed in batch " << batch << '\n';
            return EXIT_FAILURE;
        }
    }

    std::cout << "telemetry_file=" << output_path << '\n'
              << "batches=" << batches << '\n'
              << "events_per_batch=" << events_per_batch << '\n'
              << "warmup_events=" << warmup_pairs * 2 << '\n'
              << "change_batch=" << change_batch << '\n'
              << "post_matching_percent=" << post_matching_percent << '\n';
}
