#include "csv_reader.hpp"
#include "order_book.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {

void print_price(const std::optional<lob::Price>& price) {
    if (!price) {
        std::cout << "NA";
        return;
    }
    std::cout << std::fixed << std::setprecision(2) << static_cast<double>(*price) / 100.0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: order_book_replay <events.csv>\n";
        return 1;
    }

    try {
        const auto events = lob::read_events_file(argv[1]);
        lob::OrderBook book;
        std::size_t rejected = 0;
        std::size_t trade_count = 0;
        lob::Quantity traded_quantity = 0;

        const auto start = std::chrono::steady_clock::now();
        for (const auto& event : events) {
            if (event.type == lob::EventType::Add) {
                const auto result = book.submit(event.order, event.timestamp_ns);
                if (!result.accepted) ++rejected;
                trade_count += result.trades.size();
                for (const auto& trade : result.trades) traded_quantity += trade.quantity;
            } else if (!book.process(event).accepted) {
                ++rejected;
            }
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const auto top = book.top();

        std::cout << "events=" << events.size() << " rejected=" << rejected
                  << " trades=" << trade_count << " traded_quantity=" << traded_quantity
                  << " active_orders=" << book.order_count() << '\n';
        std::cout << "best_bid=";
        print_price(top.best_bid);
        std::cout << " best_ask=";
        print_price(top.best_ask);
        std::cout << " spread=";
        print_price(top.spread);
        std::cout << '\n';
        std::cout << "elapsed_seconds=" << seconds
                  << " events_per_second=" << (seconds > 0 ? events.size() / seconds : 0) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
