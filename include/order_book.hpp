#pragma once

#include "event.hpp"
#include "trade.hpp"

#include <cstddef>
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

// Numeric values are part of the logical engine contract, not a wire encoding.
enum class ResultCode : std::uint16_t {
    Accepted = 0,
    InvalidQuantity = 1,
    InvalidPrice = 2,
    DuplicateOrderId = 3,
    UnknownOrderId = 4,
    ExecutionQuantityExceedsRemaining = 5,
    UnsupportedEventType = 6,
    InternalInvariantViolation = 7,  // Reserved: internal failures throw and stop processing.
    InvalidSide = 8,
    UnknownSymbol = 9,
    QuantityOverflow = 10,
};

[[nodiscard]] const char* to_string(ResultCode code);

struct EngineResult {
    bool accepted{false};
    ResultCode code{ResultCode::InternalInvariantViolation};
    std::string message;  // Diagnostic only; callers should inspect code.
    std::vector<Trade> trades;
    Quantity resting_quantity{0};
    RequestSequence sequence{0};
    SymbolId symbol_id{1};
};

class OrderBook {
public:
    explicit OrderBook(SymbolId symbol_id = 1) : symbol_id_(symbol_id) {}
    // Stored queue iterators make a memberwise copy invalid.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = default;
    OrderBook& operator=(OrderBook&&) = default;

    EngineResult process(const Event& event);
    EngineResult cancel(OrderId id);
    EngineResult modify(OrderId id, Price new_price, Quantity new_quantity,
                        std::uint64_t timestamp_ns = 0);
    EngineResult execute(OrderId id, Quantity executed_quantity);
    EngineResult submit(Order incoming, std::uint64_t timestamp_ns);

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

    EngineResult result(ResultCode reason, std::string message,
                        std::vector<Trade> trades = {}, Quantity resting = 0) const;
    bool would_overflow(const Order& incoming, Quantity removed = 0) const;
    void add_resting(const Order& order);

    SymbolId symbol_id_;
    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, StoredOrder> orders_;
    TradeId next_trade_id_{1};
};

}  // namespace lob
