#pragma once

#include "event.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

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

class OrderBook {
public:
    ProcessResult process(const Event& event);
    ProcessResult add(const Order& order);
    ProcessResult cancel(OrderId id);

    [[nodiscard]] TopOfBook top() const;
    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }
    [[nodiscard]] std::size_t bid_level_count() const { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const { return asks_.size(); }

private:
    using Levels = std::map<Price, Quantity>;

    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Order> orders_;
};

}  // namespace lob
