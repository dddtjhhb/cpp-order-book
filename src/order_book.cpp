#include "order_book.hpp"

#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace lob {

const char* to_string(ResultCode code) {
    switch (code) {
        case ResultCode::Accepted: return "ACCEPTED";
        case ResultCode::InvalidQuantity: return "INVALID_QUANTITY";
        case ResultCode::InvalidPrice: return "INVALID_PRICE";
        case ResultCode::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
        case ResultCode::UnknownOrderId: return "UNKNOWN_ORDER_ID";
        case ResultCode::ExecutionQuantityExceedsRemaining:
            return "EXECUTION_QUANTITY_EXCEEDS_REMAINING";
        case ResultCode::UnsupportedEventType: return "UNSUPPORTED_EVENT_TYPE";
        case ResultCode::InternalInvariantViolation: return "INTERNAL_INVARIANT_VIOLATION";
    }
    return "UNKNOWN_RESULT_CODE";
}

EngineResult OrderBook::process(const Event& event) {
    EngineResult result{};
    switch (event.type) {
        case EventType::Add:
            result = submit(event.order, event.timestamp_ns);
            break;
        case EventType::Cancel:
            result = cancel(event.order.id);
            break;
        case EventType::Modify:
            result = modify(event.order.id, event.order.price, event.order.quantity,
                            event.timestamp_ns);
            break;
        case EventType::Execute:
            result = execute(event.order.id, event.order.quantity);
            break;
        default:
            result = {false, ResultCode::UnsupportedEventType, "unsupported event type", {}, 0};
            break;
    }
    result.sequence = event.sequence;
    return result;
}

EngineResult OrderBook::submit(Order incoming, std::uint64_t timestamp_ns) {
    if (incoming.quantity == 0) {
        return {false, ResultCode::InvalidQuantity, "quantity must be positive", {}, 0};
    }
    if (incoming.price <= 0) {
        return {false, ResultCode::InvalidPrice, "price must be positive", {}, 0};
    }
    if (orders_.find(incoming.id) != orders_.end()) {
        return {false, ResultCode::DuplicateOrderId, "duplicate order id", {}, 0};
    }

    std::vector<Trade> trades;
    while (incoming.quantity > 0) {
        auto& opposite = incoming.side == Side::Buy ? asks_ : bids_;
        if (opposite.empty()) break;

        const auto best = incoming.side == Side::Buy ? opposite.begin() : std::prev(opposite.end());
        const Price resting_price = best->first;
        const bool prices_cross = incoming.side == Side::Buy
            ? incoming.price >= resting_price
            : incoming.price <= resting_price;
        if (!prices_cross) break;

        const OrderId resting_id = best->second.fifo.front();
        const auto resting_found = orders_.find(resting_id);
        const Quantity traded_quantity =
            std::min(incoming.quantity, resting_found->second.order.quantity);

        trades.push_back(Trade{next_trade_id_++, incoming.id, resting_id, resting_price,
                               traded_quantity, timestamp_ns});
        incoming.quantity -= traded_quantity;
        const auto execution = execute(resting_id, traded_quantity);
        if (!execution.accepted) {
            return {false, ResultCode::InternalInvariantViolation,
                    "internal matching invariant violated", std::move(trades), 0};
        }
    }

    const Quantity remainder = incoming.quantity;
    if (remainder > 0) {
        const auto add_result = add(incoming);
        if (!add_result.accepted) {
            return {false, add_result.code, add_result.message, std::move(trades), 0};
        }
    }

    if (trades.empty()) {
        return {true, ResultCode::Accepted, "accepted as resting order", {}, remainder};
    }
    if (remainder == 0) {
        return {true, ResultCode::Accepted, "fully matched", std::move(trades), 0};
    }
    return {true, ResultCode::Accepted, "partially matched; remainder resting",
            std::move(trades), remainder};
}

EngineResult OrderBook::add(const Order& order) {
    if (order.quantity == 0) {
        return {false, ResultCode::InvalidQuantity, "quantity must be positive", {}, 0};
    }
    if (order.price <= 0) {
        return {false, ResultCode::InvalidPrice, "price must be positive", {}, 0};
    }
    if (orders_.find(order.id) != orders_.end()) {
        return {false, ResultCode::DuplicateOrderId, "duplicate order id", {}, 0};
    }

    auto& level = levels_for(order.side)[order.price];
    level.total_quantity += order.quantity;
    level.fifo.push_back(order.id);
    orders_.emplace(order.id, StoredOrder{order, std::prev(level.fifo.end())});
    return {true, ResultCode::Accepted, "added", {}, order.quantity};
}

EngineResult OrderBook::cancel(OrderId id) {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
        return {false, ResultCode::UnknownOrderId, "unknown order id", {}, 0};
    }

    erase_order(found);
    return {true, ResultCode::Accepted, "cancelled", {}, 0};
}

EngineResult OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity,
                               std::uint64_t timestamp_ns) {
    auto found = orders_.find(id);
    if (found == orders_.end()) {
        return {false, ResultCode::UnknownOrderId, "unknown order id", {}, 0};
    }
    if (new_price <= 0) {
        return {false, ResultCode::InvalidPrice, "price must be positive", {}, 0};
    }
    if (new_quantity == 0) {
        return {false, ResultCode::InvalidQuantity, "quantity must be positive", {}, 0};
    }

    const Order old = found->second.order;
    if (new_price == old.price && new_quantity <= old.quantity) {
        auto& level = levels_for(old.side).at(old.price);
        level.total_quantity -= old.quantity - new_quantity;
        found->second.order.quantity = new_quantity;
        return {true, ResultCode::Accepted, "modified in place; priority preserved", {},
                new_quantity};
    }

    erase_order(found);
    auto result = submit(Order{id, old.side, new_price, new_quantity}, timestamp_ns);
    if (!result.accepted) return result;
    if (result.trades.empty()) {
        result.message = "modified; priority reset";
    } else if (result.resting_quantity == 0) {
        result.message = "modified; fully matched";
    } else {
        result.message = "modified; partially matched; remainder resting";
    }
    return result;
}

EngineResult OrderBook::execute(OrderId id, Quantity executed_quantity) {
    auto found = orders_.find(id);
    if (found == orders_.end()) {
        return {false, ResultCode::UnknownOrderId, "unknown order id", {}, 0};
    }
    if (executed_quantity == 0) {
        return {false, ResultCode::InvalidQuantity,
                "executed quantity must be positive", {}, found->second.order.quantity};
    }
    if (executed_quantity > found->second.order.quantity) {
        return {false, ResultCode::ExecutionQuantityExceedsRemaining,
                "executed quantity exceeds remaining order quantity", {},
                found->second.order.quantity};
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
        return {true, ResultCode::Accepted, "fully executed", {}, 0};
    }
    return {true, ResultCode::Accepted, "partially executed", {}, stored.order.quantity};
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

std::optional<std::string> OrderBook::validate_invariants() const {
    std::unordered_set<OrderId> queued_ids;
    std::size_t queued_orders = 0;

    const auto validate_levels = [&](const Levels& levels, Side expected_side)
        -> std::optional<std::string> {
        for (const auto& [price, level] : levels) {
            if (level.fifo.empty()) return "empty price level";
            Quantity computed_total = 0;
            for (const OrderId id : level.fifo) {
                ++queued_orders;
                if (!queued_ids.insert(id).second) return "order appears in multiple FIFO positions";
                const auto found = orders_.find(id);
                if (found == orders_.end()) return "FIFO references missing order";
                const auto& stored = found->second;
                if (stored.order.side != expected_side) return "order side disagrees with price level";
                if (stored.order.price != price) return "order price disagrees with price level";
                if (stored.order.quantity == 0) return "resting order has zero quantity";
                if (stored.position == level.fifo.end() || *stored.position != id) {
                    return "order index stores invalid FIFO iterator";
                }
                computed_total += stored.order.quantity;
            }
            if (computed_total != level.total_quantity) return "price-level quantity mismatch";
        }
        return std::nullopt;
    };

    if (const auto error = validate_levels(bids_, Side::Buy)) return error;
    if (const auto error = validate_levels(asks_, Side::Sell)) return error;
    if (queued_orders != orders_.size()) return "order index and FIFO counts disagree";
    for (const auto& [id, stored] : orders_) {
        if (queued_ids.find(id) == queued_ids.end()) return "indexed order is absent from FIFO";
        if (stored.order.id != id) return "order index key disagrees with stored ID";
    }
    if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
        return "crossed resting book";
    }
    return std::nullopt;
}

}  // namespace lob
