#pragma once

#include "event.hpp"
#include "trade.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

struct TopOfBook {
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    std::optional<Price> spread;
};

struct ProcessResult {
    bool accepted;
    std::string message;
};

// Result of any operation that may cross the spread (new order or marketable
// modification). `resting_quantity` is what remains on the book afterwards.
struct SubmitResult {
    bool accepted;
    std::string message;
    std::vector<Trade> trades;
    Quantity resting_quantity;
};

class OrderBook {
public:
    // Applies one event. ADD and MODIFY may cross the spread, so the result
    // carries any trades the event produced.
    SubmitResult process(const Event& event);
    ProcessResult add(const Order& order);
    ProcessResult cancel(OrderId id);
    // A same-price quantity decrease keeps priority in place. Any other change
    // is treated as cancel + new order: it loses priority and, if the new price
    // is marketable, matches against the opposite side before resting.
    SubmitResult modify(OrderId id, Price new_price, Quantity new_quantity,
                        std::uint64_t timestamp_ns = 0);
    ProcessResult execute(OrderId id, Quantity executed_quantity);
    SubmitResult submit(Order incoming, std::uint64_t timestamp_ns);

    [[nodiscard]] TopOfBook top() const;
    [[nodiscard]] std::optional<Order> find_order(OrderId id) const;
    [[nodiscard]] std::vector<OrderId> fifo_at(Side side, Price price) const;
    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }
    [[nodiscard]] std::size_t bid_level_count() const { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const { return asks_.size(); }
    [[nodiscard]] std::optional<std::string> validate_invariants() const;

private:
    struct PriceLevel {
        Quantity total_quantity{0};
        std::list<OrderId> fifo;
    };

    using Levels = std::map<Price, PriceLevel>;
    using QueuePosition = std::list<OrderId>::iterator;

    struct StoredOrder {
        Order order;
        QueuePosition position;
    };

    Levels& levels_for(Side side);
    const Levels& levels_for(Side side) const;
    void erase_order(std::unordered_map<OrderId, StoredOrder>::iterator found);

    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, StoredOrder> orders_;
    TradeId next_trade_id_{1};
};

}  // namespace lob
