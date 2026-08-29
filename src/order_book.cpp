#include "order_book.hpp"

namespace lob {

ProcessResult OrderBook::process(const Event& event) {
    if (event.type == EventType::Add) {
        return add(event.order);
    }
    return cancel(event.order.id);
}

ProcessResult OrderBook::add(const Order& order) {
    if (order.quantity == 0) {
        return {false, "quantity must be positive"};
    }
    if (order.price <= 0) {
        return {false, "price must be positive"};
    }
    if (orders_.find(order.id) != orders_.end()) {
        return {false, "duplicate order id"};
    }

    auto& levels = order.side == Side::Buy ? bids_ : asks_;
    levels[order.price] += order.quantity;
    orders_.emplace(order.id, order);
    return {true, "added"};
}

ProcessResult OrderBook::cancel(OrderId id) {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
        return {false, "unknown order id"};
    }

    const Order order = found->second;
    auto& levels = order.side == Side::Buy ? bids_ : asks_;
    auto level = levels.find(order.price);
    if (level == levels.end() || level->second < order.quantity) {
        return {false, "internal quantity invariant violated"};
    }

    level->second -= order.quantity;
    if (level->second == 0) {
        levels.erase(level);
    }
    orders_.erase(found);
    return {true, "cancelled"};
}

TopOfBook OrderBook::top() const {
    TopOfBook result;
    if (!bids_.empty()) {
        result.best_bid = bids_.rbegin()->first;
    }
    if (!asks_.empty()) {
        result.best_ask = asks_.begin()->first;
    }
    if (result.best_bid && result.best_ask) {
        result.spread = *result.best_ask - *result.best_bid;
    }
    return result;
}

}  // namespace lob
