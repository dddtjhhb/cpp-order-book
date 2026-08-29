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

        const auto start = std::chrono::steady_clock::now();
        for (const auto& event : events) {
            if (!book.process(event).accepted) ++rejected;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const auto top = book.top();

        std::cout << "events=" << events.size() << " rejected=" << rejected
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
