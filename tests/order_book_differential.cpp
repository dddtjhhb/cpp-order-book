// Differential fuzzer: drives OrderBook and the naive ReferenceBook with the
// same event stream and requires identical observable behaviour after every
// event: acceptance, trades (every field), resting quantity, and the full
// depth of both sides including FIFO order.
//
// Usage:
//   order_book_differential <seed> <steps> [failure.csv]   random run
//   order_book_differential --replay <events.csv>          check a saved stream
//
// On failure the event stream is minimized and written as a replay CSV, so it
// can be fed straight back into --replay or order_book_replay.

#include "csv_reader.hpp"
#include "order_book.hpp"
#include "differential_reference_book.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using lob::Event;
using lob::EventType;
using lob::Side;

struct Mismatch {
    std::size_t step;
    std::string message;
};

const char* type_name(EventType type) {
    switch (type) {
        case EventType::Add: return "ADD";
        case EventType::Cancel: return "CANCEL";
        case EventType::Modify: return "MODIFY";
        case EventType::Execute: return "EXECUTE";
    }
    return "UNKNOWN";
}

std::string to_csv(const Event& event) {
    std::ostringstream out;
    out << event.timestamp_ns << ',' << type_name(event.type) << ',' << event.order.id << ','
        << lob::to_string(event.order.side) << ',' << event.order.price << ','
        << event.order.quantity;
    return out.str();
}

std::string describe(const lob::Trade& t) {
    std::ostringstream out;
    out << "{id=" << t.id << " in=" << t.incoming_order_id << " rest=" << t.resting_order_id
        << " px=" << t.price << " qty=" << t.quantity << " ts=" << t.timestamp_ns << '}';
    return out.str();
}

bool same_trade(const lob::Trade& a, const lob::Trade& b) {
    return a.id == b.id && a.incoming_order_id == b.incoming_order_id &&
           a.resting_order_id == b.resting_order_id && a.price == b.price &&
           a.quantity == b.quantity && a.timestamp_ns == b.timestamp_ns;
}

std::optional<std::string> compare_depth(const std::vector<lob::LevelView>& actual,
                                         const std::vector<lob::LevelView>& expected,
                                         const char* side) {
    std::ostringstream out;
    if (actual.size() != expected.size()) {
        out << side << " level count " << actual.size() << " != reference " << expected.size();
        return out.str();
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto& a = actual[i];
        const auto& e = expected[i];
        if (a.price != e.price) {
            out << side << " level " << i << " price " << a.price << " != reference " << e.price;
            return out.str();
        }
        if (a.total_quantity != e.total_quantity) {
            out << side << " @" << a.price << " total " << a.total_quantity << " != reference "
                << e.total_quantity;
            return out.str();
        }
        if (a.orders.size() != e.orders.size()) {
            out << side << " @" << a.price << " order count " << a.orders.size()
                << " != reference " << e.orders.size();
            return out.str();
        }
        for (std::size_t j = 0; j < a.orders.size(); ++j) {
            if (a.orders[j].id != e.orders[j].id || a.orders[j].quantity != e.orders[j].quantity) {
                out << side << " @" << a.price << " FIFO position " << j << ": order "
                    << a.orders[j].id << " qty " << a.orders[j].quantity << " != reference order "
                    << e.orders[j].id << " qty " << e.orders[j].quantity;
                return out.str();
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> compare_step(const lob::EngineResult& actual,
                                        const lob::EngineResult& expected) {
    std::ostringstream out;
    if (actual.accepted != expected.accepted) {
        out << "accepted=" << actual.accepted << " but reference accepted=" << expected.accepted
            << " (" << lob::to_string(actual.code) << ")";
        return out.str();
    }
    if (actual.code != expected.code) {
        out << "result code " << lob::to_string(actual.code) << " != reference "
            << lob::to_string(expected.code);
        return out.str();
    }
    if (actual.resting_quantity != expected.resting_quantity) {
        out << "resting_quantity " << actual.resting_quantity << " != reference "
            << expected.resting_quantity;
        return out.str();
    }
    const auto n = std::max(actual.trades.size(), expected.trades.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= actual.trades.size()) {
            out << "missing trade " << i << ": reference " << describe(expected.trades[i]);
            return out.str();
        }
        if (i >= expected.trades.size()) {
            out << "extra trade " << i << ": " << describe(actual.trades[i]);
            return out.str();
        }
        if (!same_trade(actual.trades[i], expected.trades[i])) {
            out << "trade " << i << ' ' << describe(actual.trades[i]) << " != reference "
                << describe(expected.trades[i]);
            return out.str();
        }
    }
    return std::nullopt;
}

std::optional<Mismatch> run(const std::vector<Event>& events) {
    lob::OrderBook book;
    lob::testing::ReferenceBook reference;
    for (std::size_t step = 0; step < events.size(); ++step) {
        const auto actual = book.process(events[step]);
        const auto expected = reference.process(events[step]);
        if (auto error = compare_step(actual, expected)) return Mismatch{step, *error};
        if (auto error = compare_depth(book.depth(Side::Buy), reference.depth(Side::Buy), "bid")) {
            return Mismatch{step, *error};
        }
        if (auto error = compare_depth(book.depth(Side::Sell), reference.depth(Side::Sell), "ask")) {
            return Mismatch{step, *error};
        }
        if (book.order_count() != reference.order_count()) {
            return Mismatch{step, "order_count disagrees with reference"};
        }
        if (auto error = book.validate_invariants()) return Mismatch{step, "invariant: " + *error};
    }
    return std::nullopt;
}

// Greedy delta debugging: drop chunks while the stream still fails.
std::vector<Event> minimize(std::vector<Event> events) {
    std::size_t chunk = std::max<std::size_t>(1, events.size() / 2);
    while (true) {
        bool reduced = false;
        for (std::size_t start = 0; start + chunk <= events.size(); ++start) {
            std::vector<Event> candidate;
            candidate.reserve(events.size() - chunk);
            candidate.insert(candidate.end(), events.begin(), events.begin() + start);
            candidate.insert(candidate.end(), events.begin() + start + chunk, events.end());
            if (!candidate.empty() && run(candidate)) {
                events = std::move(candidate);
                reduced = true;
                break;
            }
        }
        if (reduced) {
            chunk = std::min(chunk, std::max<std::size_t>(1, events.size() / 2));
        } else if (chunk == 1) {
            break;
        } else {
            chunk = std::max<std::size_t>(1, chunk / 2);
        }
    }
    // Renumber timestamps so the saved reproducer reads 1, 2, 3, ...
    for (std::size_t i = 0; i < events.size(); ++i) events[i].timestamp_ns = i + 1;
    return events;
}

// The generator tracks state with the *reference* book, so the implementation
// under test never decides which events get generated.
class Generator {
public:
    explicit Generator(std::uint64_t seed) : random_(seed) {}

    Event next(std::uint64_t timestamp) {
        const auto active = model_.active_ids();
        const int roll = static_cast<int>(random_() % 100);
        Event event = choose(roll, active);
        event.timestamp_ns = timestamp;
        model_.process(event);
        return event;
    }

private:
    lob::Price price() { return 9990 + static_cast<lob::Price>(random_() % 21); }
    lob::Quantity quantity() { return 1 + random_() % 50; }
    Side side() { return random_() % 2 == 0 ? Side::Buy : Side::Sell; }
    template <typename T>
    const T& pick(const std::vector<T>& values) { return values[random_() % values.size()]; }

    Event choose(int roll, const std::vector<lob::OrderId>& active) {
        if (active.empty() || roll < 40) {  // new order, usually valid
            const auto id = next_id_++;
            return {0, EventType::Add, {id, side(), price(), quantity()}};
        }
        const auto id = pick(active);
        const auto stored = *model_.find_order(id);
        if (roll < 44) {  // invalid ADD: duplicate id, zero quantity, or non-positive price
            switch (random_() % 3) {
                case 0: return {0, EventType::Add, {id, side(), price(), quantity()}};
                case 1: return {0, EventType::Add, {next_id_++, side(), price(), 0}};
                default:
                    return {0, EventType::Add,
                            {next_id_++, side(), -static_cast<lob::Price>(random_() % 2), 1}};
            }
        }
        if (roll < 58) return {0, EventType::Cancel, stored};
        if (roll < 61) return {0, EventType::Cancel, {next_id_ + 1000, side(), 0, 0}};
        if (roll < 81) {  // modify: shrink in place, same size elsewhere, or anything in band
            switch (random_() % 3) {
                case 0:
                    return {0, EventType::Modify,
                            {id, stored.side, stored.price, 1 + random_() % stored.quantity}};
                case 1: return {0, EventType::Modify, {id, stored.side, price(), stored.quantity}};
                default: return {0, EventType::Modify, {id, stored.side, price(), quantity()}};
            }
        }
        if (roll < 84) {  // invalid modify
            return random_() % 2 == 0
                ? Event{0, EventType::Modify, {id, stored.side, price(), 0}}
                : Event{0, EventType::Modify, {id, stored.side, 0, quantity()}};
        }
        if (roll < 96) {  // execute: partial or full
            return {0, EventType::Execute,
                    {id, stored.side, stored.price, 1 + random_() % stored.quantity}};
        }
        return {0, EventType::Execute,  // over-execution must be rejected
                {id, stored.side, stored.price, stored.quantity + 1 + random_() % 5}};
    }

    std::mt19937_64 random_;
    lob::testing::ReferenceBook model_;
    lob::OrderId next_id_{1};
};

// What a passing run actually exercised, so "passed" is not vacuous.
std::string coverage(const std::vector<Event>& events) {
    lob::OrderBook book;
    std::size_t accepted[4]{}, rejected[4]{}, trades = 0, crossing_modifies = 0, sweeps = 0,
                max_orders = 0;
    for (const auto& event : events) {
        const auto result = book.process(event);
        const auto type = static_cast<std::size_t>(event.type);
        ++(result.accepted ? accepted : rejected)[type];
        trades += result.trades.size();
        if (event.type == EventType::Modify && !result.trades.empty()) ++crossing_modifies;
        if (result.trades.size() > 1) ++sweeps;
        max_orders = std::max(max_orders, book.order_count());
    }
    std::ostringstream out;
    for (const auto type : {EventType::Add, EventType::Cancel, EventType::Modify,
                            EventType::Execute}) {
        const auto i = static_cast<std::size_t>(type);
        out << type_name(type) << "=" << accepted[i] << "/" << rejected[i] << "(ok/rej) ";
    }
    out << "trades=" << trades << " multi_trade_events=" << sweeps
        << " crossing_modifies=" << crossing_modifies << " max_resting_orders=" << max_orders;
    return out.str();
}

void save(const std::string& path, const std::vector<Event>& events, const Mismatch& mismatch,
          const std::string& origin) {
    std::ofstream out(path);
    out << "timestamp_ns,event_type,order_id,side,price_ticks,quantity\n";
    for (const auto& event : events) out << to_csv(event) << '\n';
    std::ofstream note(path + ".txt");
    note << origin << "\nfailing_step=" << mismatch.step << "\nreason=" << mismatch.message << '\n';
}

int report(const std::vector<Event>& events, const Mismatch& mismatch, const std::string& path,
           const std::string& origin) {
    const auto minimized = minimize(events);
    const auto minimized_mismatch = *run(minimized);
    save(path, minimized, minimized_mismatch, origin);
    std::cerr << "differential mismatch at step " << mismatch.step << ": " << mismatch.message
              << "\nminimized from " << events.size() << " to " << minimized.size()
              << " events (" << minimized_mismatch.message << "); saved to " << path << '\n';
    for (const auto& event : minimized) std::cerr << "  " << to_csv(event) << '\n';
    return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--replay") {
            const auto events = lob::read_events_file(argv[2]);
            if (const auto mismatch = run(events)) {
                return report(events, *mismatch, "differential_replay_failure.csv",
                              std::string("replay=") + argv[2]);
            }
            std::cout << "differential_replay_passed file=" << argv[2]
                      << " events=" << events.size() << '\n';
            return EXIT_SUCCESS;
        }

        const std::uint64_t seed = argc > 1 ? std::stoull(argv[1]) : 1;
        const std::size_t steps = argc > 2 ? std::stoull(argv[2]) : 10000;
        const std::string path = argc > 3 ? argv[3] : "differential_failure.csv";
        if (steps == 0) {
            std::cerr << "usage: order_book_differential <seed> <steps>0> [failure.csv]\n"
                         "       order_book_differential --replay <events.csv>\n";
            return EXIT_FAILURE;
        }

        Generator generator(seed);
        std::vector<Event> events;
        events.reserve(steps);
        for (std::size_t i = 0; i < steps; ++i) events.push_back(generator.next(i + 1));

        if (const auto mismatch = run(events)) {
            return report(events, *mismatch, path, "seed=" + std::to_string(seed));
        }
        std::cout << "differential_passed seed=" << seed << " steps=" << steps << '\n'
                  << "  " << coverage(events) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
