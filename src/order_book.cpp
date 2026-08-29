#include "order_book.hpp"

#include <iterator>

namespace lob {

ProcessResult OrderBook::process(const Event& event) {
    switch (event.type) {
        case EventType::Add:
            return add(event.order);
        case EventType::Cancel:
            return cancel(event.order.id);
        case EventType::Modify:
            return modify(event.order.id, event.order.price, event.order.quantity);
        case EventType::Execute:
            return execute(event.order.id, event.order.quantity);
    }
    return {false, "unsupported event type"};
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

    auto& level = levels_for(order.side)[order.price];
    level.total_quantity += order.quantity;
    level.fifo.push_back(order.id);
    orders_.emplace(order.id, StoredOrder{order, std::prev(level.fifo.end())});
    return {true, "added"};
}

ProcessResult OrderBook::cancel(OrderId id) {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
        return {false, "unknown order id"};
    }

    erase_order(found);
    return {true, "cancelled"};
}

ProcessResult OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    auto found = orders_.find(id);
    if (found == orders_.end()) return {false, "unknown order id"};
    if (new_price <= 0) return {false, "price must be positive"};
    if (new_quantity == 0) return {false, "quantity must be positive"};

    const Order old = found->second.order;
    if (new_price == old.price && new_quantity <= old.quantity) {
        auto& level = levels_for(old.side).at(old.price);
        level.total_quantity -= old.quantity - new_quantity;
        found->second.order.quantity = new_quantity;
        return {true, "modified in place; priority preserved"};
    }

    erase_order(found);
    const auto result = add(Order{id, old.side, new_price, new_quantity});
    return result.accepted ? ProcessResult{true, "modified; priority reset"} : result;
}

ProcessResult OrderBook::execute(OrderId id, Quantity executed_quantity) {
    auto found = orders_.find(id);
    if (found == orders_.end()) return {false, "unknown order id"};
    if (executed_quantity == 0) return {false, "executed quantity must be positive"};
    if (executed_quantity > found->second.order.quantity) {
        return {false, "executed quantity exceeds remaining order quantity"};
    }

    auto& stored = found->second;
    auto& levels = levels_for(stored.order.side);
    auto level_it = levels.find(stored.order.price);
    auto& level = level_it->second;
    level.total_quantity -= executed_quantity;
    stored.order.quantity -= executed_quantity;
    if (stored.order.quantity == 0) {
        level.fifo.erase(stored.position);
        if (level.fifo.empty()) levels.erase(level_it);
        orders_.erase(found);
        return {true, "fully executed"};
    }
    return {true, "partially executed"};
}

std::optional<Order> OrderBook::find_order(OrderId id) const {
    const auto found = orders_.find(id);
    if (found == orders_.end()) return std::nullopt;
    return found->second.order;
}

std::vector<OrderId> OrderBook::fifo_at(Side side, Price price) const {
    const auto& levels = levels_for(side);
    const auto found = levels.find(price);
    if (found == levels.end()) return {};
    return {found->second.fifo.begin(), found->second.fifo.end()};
}

OrderBook::Levels& OrderBook::levels_for(Side side) {
    return side == Side::Buy ? bids_ : asks_;
}

const OrderBook::Levels& OrderBook::levels_for(Side side) const {
    return side == Side::Buy ? bids_ : asks_;
}

void OrderBook::erase_order(std::unordered_map<OrderId, StoredOrder>::iterator found) {
    const Order order = found->second.order;
    auto& levels = levels_for(order.side);
    auto level = levels.find(order.price);
    level->second.total_quantity -= order.quantity;
    level->second.fifo.erase(found->second.position);
    if (level->second.fifo.empty()) levels.erase(level);
    orders_.erase(found);
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
