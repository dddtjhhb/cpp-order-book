#include "order_book.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
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
        case ResultCode::InvalidSide: return "INVALID_SIDE";
        case ResultCode::UnknownSymbol: return "UNKNOWN_SYMBOL";
        case ResultCode::QuantityOverflow: return "QUANTITY_OVERFLOW";
    }
    return "UNKNOWN_RESULT_CODE";
}

EngineResult OrderBook::result(ResultCode reason, std::string message,
                               std::vector<Trade> trades, Quantity resting) const {
    return {reason == ResultCode::Accepted, reason, std::move(message),
            std::move(trades), resting, 0, symbol_id_};
}

EngineResult OrderBook::process(const Event& event) {
    auto output = [&]() -> EngineResult {
        if (event.symbol_id != symbol_id_) {
            return result(ResultCode::UnknownSymbol, "unknown symbol");
        }
        switch (event.type) {
            case EventType::Add:
                return submit(event.order, event.timestamp_ns);
            case EventType::Cancel:
                return cancel(event.order.id);
            case EventType::Modify:
                return modify(event.order.id, event.order.price, event.order.quantity,
                              event.timestamp_ns);
            case EventType::Execute:
                return execute(event.order.id, event.order.quantity);
        }
        return result(ResultCode::UnsupportedEventType, "unsupported event type");
    }();
    output.symbol_id = event.symbol_id;
    output.sequence = event.sequence;
    return output;
}

bool OrderBook::would_overflow(const Order& incoming, Quantity removed) const {
    const auto& levels = levels_for(incoming.side);
    const auto found = levels.find(incoming.price);
    const Quantity current = found == levels.end() ? 0 : found->second.total_quantity;
    return incoming.quantity > std::numeric_limits<Quantity>::max() - (current - removed);
}

EngineResult OrderBook::submit(Order incoming, std::uint64_t timestamp_ns) {
    if (incoming.side != Side::Buy && incoming.side != Side::Sell) {
        return result(ResultCode::InvalidSide, "invalid side");
    }
    if (incoming.quantity == 0) {
        return result(ResultCode::InvalidQuantity, "quantity must be positive");
    }
    if (incoming.price <= 0) {
        return result(ResultCode::InvalidPrice, "price must be positive");
    }
    if (orders_.find(incoming.id) != orders_.end()) {
        return result(ResultCode::DuplicateOrderId, "duplicate order id");
    }
    // An existing same-side level cannot cross the opposite book. Thus this
    // check can reject overflow before any fills mutate state.
    if (would_overflow(incoming)) {
        return result(ResultCode::QuantityOverflow, "price-level quantity would overflow");
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
            throw std::logic_error("internal matching invariant violated");
        }
    }

    const Quantity remainder = incoming.quantity;
    if (remainder > 0) add_resting(incoming);

    if (trades.empty()) return result(ResultCode::Accepted, "accepted as resting order", {}, remainder);
    if (remainder == 0) return result(ResultCode::Accepted, "fully matched", std::move(trades));
    return result(ResultCode::Accepted, "partially matched; remainder resting", std::move(trades), remainder);
}

void OrderBook::add_resting(const Order& order) {
    auto& level = levels_for(order.side)[order.price];
    level.total_quantity += order.quantity;
    level.fifo.push_back(order.id);
    orders_.emplace(order.id, StoredOrder{order, std::prev(level.fifo.end())});
}

EngineResult OrderBook::cancel(OrderId id) {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
        return result(ResultCode::UnknownOrderId, "unknown order id");
    }

    erase_order(found);
    return result(ResultCode::Accepted, "cancelled");
}

EngineResult OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity,
                               std::uint64_t timestamp_ns) {
    auto found = orders_.find(id);
    if (found == orders_.end()) return result(ResultCode::UnknownOrderId, "unknown order id");
    if (new_price <= 0) return result(ResultCode::InvalidPrice, "price must be positive");
    if (new_quantity == 0) return result(ResultCode::InvalidQuantity, "quantity must be positive");

    const Order old = found->second.order;
    const Order replacement{id, old.side, new_price, new_quantity};
    if (would_overflow(replacement, new_price == old.price ? old.quantity : 0)) {
        return result(ResultCode::QuantityOverflow, "price-level quantity would overflow");
    }
    if (new_price == old.price && new_quantity <= old.quantity) {
        auto& level = levels_for(old.side).at(old.price);
        level.total_quantity -= old.quantity - new_quantity;
        found->second.order.quantity = new_quantity;
        return result(ResultCode::Accepted, "modified in place; priority preserved", {}, new_quantity);
    }

    // All business validation precedes removal: a rejected replacement leaves
    // the original quantity, price and FIFO priority unchanged.
    erase_order(found);
    return submit(replacement, timestamp_ns);
}

EngineResult OrderBook::execute(OrderId id, Quantity executed_quantity) {
    auto found = orders_.find(id);
    if (found == orders_.end()) return result(ResultCode::UnknownOrderId, "unknown order id");
    if (executed_quantity == 0) return result(ResultCode::InvalidQuantity, "executed quantity must be positive");
    if (executed_quantity > found->second.order.quantity) {
        return result(ResultCode::ExecutionQuantityExceedsRemaining, "executed quantity exceeds remaining order quantity");
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
        return result(ResultCode::Accepted, "fully executed");
    }
    return result(ResultCode::Accepted, "partially executed", {}, stored.order.quantity);
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
                if (stored.order.quantity > std::numeric_limits<Quantity>::max() - computed_total) {
                    return "price-level quantity overflow";
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
