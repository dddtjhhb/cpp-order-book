// Engine benchmark on a deep book with a mixed, realistic-shaped workload.
//
// What is measured: OrderBook::process() only. Event generation, CSV parsing,
// I/O and the book pre-fill are outside every timed region.
//
// Workload (deterministic for a given seed):
//   * pre-fill: `depth` resting orders per side spread over `levels` price
//     levels, denser near the touch;
//   * then `events` requests, roughly 45% passive ADD, 35% CANCEL,
//     10% MODIFY (half same-price size-down, half passive re-price) and 10%
//     aggressive ADD that crosses the spread and sweeps one or more orders;
//     the mix adapts slightly so the book stays near its pre-filled size.
//
// Two separate kinds of run, so per-event timing never pollutes throughput:
//   * throughput runs: one clock read before and after the whole replay;
//   * latency runs: a clock read around every process() call, split by
//     operation type. The clock's own cost and resolution are measured and
//     printed, because on Apple Silicon steady_clock ticks every ~41.7 ns,
//     which quantizes the lowest percentiles.

#include "order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using lob::Event;
using lob::EventType;
using lob::OrderId;
using lob::Price;
using lob::Side;

enum class Kind : std::uint8_t { PassiveAdd, Cancel, Modify, AggressiveAdd, Count };
constexpr const char* kKindNames[] = {"passive_add", "cancel", "modify", "aggressive_add"};
constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::Count);

struct Config {
    std::size_t depth = 5000;     // resting orders per side after pre-fill
    std::size_t levels = 500;     // price levels per side the book spans
    std::size_t events = 1000000; // timed requests per run
    std::size_t throughput_runs = 7;
    std::size_t latency_runs = 3;
    std::uint64_t seed = 1;
    std::string markdown_path;
    std::string csv_path;
};

struct Workload {
    std::vector<Event> prefill;
    std::vector<Event> events;
    std::vector<Kind> kinds;
    std::size_t trades = 0;
    std::size_t multi_level_sweeps = 0;
    std::size_t final_orders = 0;
};

// Generates the workload by driving a model book, so every CANCEL/MODIFY
// targets a live order and every "passive" ADD really rests.
class Generator {
public:
    Generator(const Config& config) : config_(config), random_(config.seed) {}

    Workload build() {
        Workload w;
        const Price mid = 1'000'000;
        for (std::size_t i = 0; i < config_.depth * 2; ++i) {
            const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
            const Price offset = 1 + static_cast<Price>(depth_offset());
            const Price price = side == Side::Buy ? mid - offset : mid + offset;
            w.prefill.push_back(make(EventType::Add, next_id_++, side, price, size()));
            apply(w.prefill.back(), nullptr);
        }
        const std::size_t target = config_.depth * 2;
        w.events.reserve(config_.events);
        w.kinds.reserve(config_.events);
        for (std::size_t i = 0; i < config_.events; ++i) {
            // Keep the book near its target size: more cancels when it grows.
            const double drift = (static_cast<double>(live_.size()) - target) / target;
            const double cancel_share = std::clamp(0.35 + drift, 0.15, 0.60);
            const double roll = unit();
            Kind kind;
            if (live_.empty()) kind = Kind::PassiveAdd;
            else if (roll < cancel_share) kind = Kind::Cancel;
            else if (roll < cancel_share + 0.10) kind = Kind::Modify;
            else if (roll < cancel_share + 0.20) kind = Kind::AggressiveAdd;
            else kind = Kind::PassiveAdd;

            Event event = next(kind);
            w.kinds.push_back(kind);
            const auto result = apply(event, &w);
            if (!result.accepted) {
                std::cerr << "generator produced a rejected event: " << result.message << '\n';
                std::exit(EXIT_FAILURE);
            }
            w.events.push_back(event);
        }
        w.final_orders = model_.order_count();
        return w;
    }

private:
    Event make(EventType type, OrderId id, Side side, Price price, lob::Quantity quantity) {
        ++timestamp_;
        return {timestamp_, type, {id, side, price, quantity}, 1, timestamp_};
    }

    double unit() { return std::uniform_real_distribution<double>(0.0, 1.0)(random_); }
    lob::Quantity size() { return 1 + random_() % 100; }

    // Geometric-ish distance from the touch: most liquidity sits near the top.
    std::size_t depth_offset() {
        const double x = std::exponential_distribution<double>(8.0 / config_.levels)(random_);
        return std::min<std::size_t>(config_.levels - 1, static_cast<std::size_t>(x));
    }

    OrderId random_live() { return live_[random_() % live_.size()]; }

    Event next(Kind kind) {
        const auto top = model_.top();
        switch (kind) {
            case Kind::PassiveAdd: {
                const Side side = random_() % 2 == 0 ? Side::Buy : Side::Sell;
                return make(EventType::Add, next_id_++, side, passive_price(side, top), size());
            }
            case Kind::Cancel:
                return make(EventType::Cancel, random_live(), Side::Buy, 0, 0);
            case Kind::Modify: {
                const OrderId id = random_live();
                const auto order = *model_.find_order(id);
                if (random_() % 2 == 0 && order.quantity > 1) {  // size down, keep priority
                    return make(EventType::Modify, id, order.side, order.price,
                                1 + random_() % (order.quantity - 1));
                }
                return make(EventType::Modify, id, order.side, passive_price(order.side, top),
                            order.quantity);
            }
            case Kind::AggressiveAdd: {
                const Side side = random_() % 2 == 0 ? Side::Buy : Side::Sell;
                const auto best = side == Side::Buy ? top.best_ask : top.best_bid;
                if (!best) return make(EventType::Add, next_id_++, side,
                                       passive_price(side, top), size());
                // Limit a few ticks through the touch; size up to ~2 orders' worth.
                const Price through = static_cast<Price>(random_() % 3);
                const Price price = side == Side::Buy ? *best + through : *best - through;
                return make(EventType::Add, next_id_++, side, price, 1 + random_() % 200);
            }
            case Kind::Count: break;
        }
        std::abort();
    }

    // A price on `side` at or behind that side's best, never crossing.
    Price passive_price(Side side, const lob::TopOfBook& top) {
        const Price mid = 1'000'000;
        const Price offset = static_cast<Price>(depth_offset());  // 0 = join the touch
        if (side == Side::Buy) {
            const Price base = top.best_bid ? *top.best_bid : mid - 1;
            const Price cap = top.best_ask ? *top.best_ask - 1 : base;
            return std::max<Price>(1, std::min(cap, base - offset));
        }
        const Price base = top.best_ask ? *top.best_ask : mid + 1;
        const Price floor = top.best_bid ? *top.best_bid + 1 : base;
        return std::max(floor, base + offset);
    }

    lob::EngineResult apply(const Event& event, Workload* w) {
        auto result = model_.process(event);
        if (event.type == EventType::Add && result.resting_quantity > 0) add_live(event.order.id);
        if (event.type == EventType::Cancel) remove_live(event.order.id);
        if (event.type == EventType::Modify && !model_.find_order(event.order.id)) {
            remove_live(event.order.id);
        }
        std::map<Price, int> levels_hit;
        for (const auto& trade : result.trades) {
            levels_hit[trade.price] = 1;
            if (!model_.find_order(trade.resting_order_id)) remove_live(trade.resting_order_id);
        }
        if (w) {
            w->trades += result.trades.size();
            if (levels_hit.size() > 1) ++w->multi_level_sweeps;
        }
        return result;
    }

    void add_live(OrderId id) {
        index_[id] = live_.size();
        live_.push_back(id);
    }
    void remove_live(OrderId id) {
        const auto found = index_.find(id);
        if (found == index_.end()) return;
        const std::size_t at = found->second;
        live_[at] = live_.back();
        index_[live_[at]] = at;
        live_.pop_back();
        index_.erase(id);
    }

    const Config& config_;
    std::mt19937_64 random_;
    lob::OrderBook model_;
    std::vector<OrderId> live_;
    std::unordered_map<OrderId, std::size_t> index_;
    OrderId next_id_ = 1;
    std::uint64_t timestamp_ = 0;
};

double percentile(std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    const double position = q * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return percentile(values, 0.5);
}

void prefill(lob::OrderBook& book, const Workload& w) {
    for (const auto& event : w.prefill) book.process(event);
}

// Measured clock properties, reported next to the latencies they limit.
struct ClockInfo {
    double read_cost_ns;   // median cost of one steady_clock::now() pair, amortized
    double resolution_ns;  // smallest non-zero difference between two reads
};

ClockInfo measure_clock() {
    constexpr int kReads = 1'000'000;
    const auto start = Clock::now();
    Clock::time_point last{};
    for (int i = 0; i < kReads; ++i) last = Clock::now();
    const double cost = std::chrono::duration<double, std::nano>(last - start).count() / kReads;
    double resolution = 1e9;
    for (int i = 0; i < 100000; ++i) {
        const auto a = Clock::now();
        auto b = Clock::now();
        while (b == a) b = Clock::now();
        resolution = std::min(resolution, std::chrono::duration<double, std::nano>(b - a).count());
    }
    return {cost, resolution};
}

std::string compiler() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

[[noreturn]] void usage() {
    std::cerr << "usage: order_book_engine_benchmark [--events N] [--depth N] [--levels N]\n"
                 "         [--runs N] [--latency-runs N] [--seed N]\n"
                 "         [--markdown results.md] [--csv results.csv]\n";
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) usage();
        const std::string value = argv[++i];
        if (arg == "--events") config.events = std::stoull(value);
        else if (arg == "--depth") config.depth = std::stoull(value);
        else if (arg == "--levels") config.levels = std::max<std::size_t>(2, std::stoull(value));
        else if (arg == "--runs") config.throughput_runs = std::max<std::size_t>(1, std::stoull(value));
        else if (arg == "--latency-runs") config.latency_runs = std::max<std::size_t>(1, std::stoull(value));
        else if (arg == "--seed") config.seed = std::stoull(value);
        else if (arg == "--markdown") config.markdown_path = value;
        else if (arg == "--csv") config.csv_path = value;
        else usage();
    }

    std::cerr << "generating workload...\n";
    const Workload w = Generator(config).build();
    std::size_t kind_counts[kKinds] = {};
    for (const auto kind : w.kinds) ++kind_counts[static_cast<std::size_t>(kind)];

    // Warm-up pass (untimed): faults in code and allocator paths.
    {
        lob::OrderBook book;
        prefill(book, w);
        for (const auto& event : w.events) book.process(event);
    }

    // Throughput runs.
    std::vector<double> events_per_second;
    std::size_t sink = 0;
    for (std::size_t run = 0; run < config.throughput_runs; ++run) {
        lob::OrderBook book;
        prefill(book, w);
        const auto start = Clock::now();
        for (const auto& event : w.events) sink += book.process(event).trades.size();
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        events_per_second.push_back(static_cast<double>(w.events.size()) / seconds);
        if (book.validate_invariants()) {
            std::cerr << "invariant violated after run\n";
            return EXIT_FAILURE;
        }
    }
    std::sort(events_per_second.begin(), events_per_second.end());

    // Latency runs, pooled per operation type.
    std::vector<double> latency[kKinds + 1];  // last = all
    for (auto& samples : latency) samples.reserve(w.events.size() * config.latency_runs / 2);
    for (std::size_t run = 0; run < config.latency_runs; ++run) {
        lob::OrderBook book;
        prefill(book, w);
        for (std::size_t i = 0; i < w.events.size(); ++i) {
            const auto start = Clock::now();
            const auto result = book.process(w.events[i]);
            const auto end = Clock::now();
            sink += result.trades.size();
            const double ns = std::chrono::duration<double, std::nano>(end - start).count();
            latency[static_cast<std::size_t>(w.kinds[i])].push_back(ns);
            latency[kKinds].push_back(ns);
        }
    }
    for (auto& samples : latency) std::sort(samples.begin(), samples.end());
    const ClockInfo clock = measure_clock();

    // ---- Report ------------------------------------------------------------
    std::ostringstream md;
    md << std::fixed;
    md << "## Engine benchmark\n\n";
    md << "| Setting | Value |\n|---|---|\n";
    md << "| Compiler | " << compiler() << " |\n";
#ifdef LOB_BENCH_BUILD_FLAGS
    md << "| Build flags | `" << LOB_BENCH_BUILD_FLAGS << "` |\n";
#endif
    md << "| Book pre-fill | " << config.depth << " orders/side over " << config.levels
       << " levels/side |\n";
    md << "| Timed events per run | " << w.events.size() << " (";
    for (std::size_t k = 0; k < kKinds; ++k) {
        md << (k ? ", " : "") << kKindNames[k] << ' ' << std::setprecision(1)
           << 100.0 * kind_counts[k] / w.events.size() << '%';
    }
    md << ") |\n";
    md << "| Trades per run | " << w.trades << " (" << w.multi_level_sweeps
       << " multi-level sweeps) |\n";
    md << "| Resting orders at end | " << w.final_orders << " |\n";
    md << "| Runs | " << config.throughput_runs << " throughput, " << config.latency_runs
       << " latency (pooled), after 1 warm-up |\n";
    md << "| Clock | steady_clock, resolution " << std::setprecision(1) << clock.resolution_ns
       << " ns, read cost " << clock.read_cost_ns << " ns |\n\n";

    md << "### Throughput (no per-event timing)\n\n";
    md << "| Median | Min | Max | Mean time per event |\n|---:|---:|---:|---:|\n";
    const double med = median(events_per_second);
    md << std::setprecision(2) << "| " << med / 1e6 << " M events/s | "
       << events_per_second.front() / 1e6 << " M | " << events_per_second.back() / 1e6 << " M | "
       << std::setprecision(1) << 1e9 / med << " ns |\n\n";

    md << "### Per-event latency (ns, includes one clock read)\n\n";
    md << "| Operation | Samples | p50 | p90 | p99 | p99.9 | max |\n"
          "|---|---:|---:|---:|---:|---:|---:|\n";
    std::ostringstream csv;
    csv << "operation,samples,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n";
    for (std::size_t k = 0; k <= kKinds; ++k) {
        auto& samples = latency[k];
        const std::string name = k == kKinds ? "**all**" : kKindNames[k];
        md << std::setprecision(0) << "| " << name << " | " << samples.size() << " | "
           << percentile(samples, 0.50) << " | " << percentile(samples, 0.90) << " | "
           << percentile(samples, 0.99) << " | " << percentile(samples, 0.999) << " | "
           << (samples.empty() ? 0.0 : samples.back()) << " |\n";
        csv << (k == kKinds ? "all" : kKindNames[k]) << ',' << samples.size() << ','
            << percentile(samples, 0.50) << ',' << percentile(samples, 0.90) << ','
            << percentile(samples, 0.99) << ',' << percentile(samples, 0.999) << ','
            << (samples.empty() ? 0.0 : samples.back()) << '\n';
    }
    md << "\nThroughput " << std::setprecision(2) << "min/max spread: "
       << 100.0 * (events_per_second.back() - events_per_second.front()) / med << "% of median. "
       << "Latencies below ~" << std::setprecision(0) << 2 * clock.resolution_ns
       << " ns are quantized by the clock. `max` reflects OS scheduling and is not a property "
          "of the engine.\n";

    std::cout << md.str();
    if (!config.markdown_path.empty()) std::ofstream(config.markdown_path) << md.str();
    if (!config.csv_path.empty()) std::ofstream(config.csv_path) << csv.str();
    return sink == 0 ? EXIT_FAILURE : EXIT_SUCCESS;  // sink keeps work observable
}
