#pragma once

// A deliberately naive order book used only as a test oracle.
//
// Every resting order lives in one flat vector with an insertion sequence
// number. Matching, lookup, and depth are linear scans. Nothing here is
// optimized, so the code can be checked by reading it: price-time priority is
// literally "best price, then smallest sequence number".
//
// It shares only the plain data types (Order, Trade, Event, LevelView) with
// OrderBook, never its logic.

#include "order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace lob::testing {

class ReferenceBook {
public:
    EngineResult process(const Event& event) {
        EngineResult result;
        switch (event.type) {
            case EventType::Add: result = submit(event.order, event.timestamp_ns); break;
            case EventType::Cancel: result = cancel(event.order.id); break;
            case EventType::Modify:
                result = modify(event.order.id, event.order.price, event.order.quantity,
                                event.timestamp_ns);
                break;
            case EventType::Execute: result = execute(event.order.id, event.order.quantity); break;
        }
        result.sequence = event.sequence;
        result.symbol_id = event.symbol_id;
        return result;
    }

    [[nodiscard]] std::optional<Order> find_order(OrderId id) const {
        const auto* resting = find(id);
        if (!resting) return std::nullopt;
        return resting->order;
    }

    [[nodiscard]] std::vector<OrderId> active_ids() const {
        std::vector<OrderId> ids;
        for (const auto& resting : resting_) ids.push_back(resting.order.id);
        return ids;
    }

    [[nodiscard]] std::size_t order_count() const { return resting_.size(); }

    [[nodiscard]] std::optional<Price> best(Side side) const {
        std::optional<Price> result;
        for (const auto& resting : resting_) {
            if (resting.order.side != side) continue;
            if (!result || better(side, resting.order.price, *result)) result = resting.order.price;
        }
        return result;
    }

    [[nodiscard]] std::vector<LevelView> depth(Side side) const {
        std::vector<Resting> same_side;
        for (const auto& resting : resting_) {
            if (resting.order.side == side) same_side.push_back(resting);
        }
        std::sort(same_side.begin(), same_side.end(), [side](const Resting& a, const Resting& b) {
            if (a.order.price != b.order.price) return better(side, a.order.price, b.order.price);
            return a.sequence < b.sequence;
        });
        std::vector<LevelView> levels;
        for (const auto& resting : same_side) {
            if (levels.empty() || levels.back().price != resting.order.price) {
                levels.push_back({resting.order.price, 0, {}});
            }
            levels.back().total_quantity += resting.order.quantity;
            levels.back().orders.push_back(resting.order);
        }
        return levels;
    }

private:
    struct Resting {
        Order order;
        std::uint64_t sequence;  // smaller = earlier = higher time priority
    };

    static EngineResult reject(ResultCode code) {
        return {false, code, {}, {}, 0, 0, 0};
    }

    static bool better(Side side, Price a, Price b) {
        return side == Side::Buy ? a > b : a < b;
    }

    Resting* find(OrderId id) {
        for (auto& resting : resting_) {
            if (resting.order.id == id) return &resting;
        }
        return nullptr;
    }
    const Resting* find(OrderId id) const {
        for (const auto& resting : resting_) {
            if (resting.order.id == id) return &resting;
        }
        return nullptr;
    }

    void remove(OrderId id) {
        resting_.erase(std::remove_if(resting_.begin(), resting_.end(),
                                      [id](const Resting& r) { return r.order.id == id; }),
                       resting_.end());
    }

    // Highest-priority resting order on `side`: best price, then earliest sequence.
    Resting* best_resting(Side side) {
        Resting* best = nullptr;
        for (auto& resting : resting_) {
            if (resting.order.side != side) continue;
            if (!best || better(side, resting.order.price, best->order.price) ||
                (resting.order.price == best->order.price && resting.sequence < best->sequence)) {
                best = &resting;
            }
        }
        return best;
    }

    EngineResult submit(Order incoming, std::uint64_t timestamp_ns) {
        if (incoming.quantity == 0) return reject(ResultCode::InvalidQuantity);
        if (incoming.price <= 0) return reject(ResultCode::InvalidPrice);
        if (find(incoming.id)) return reject(ResultCode::DuplicateOrderId);

        EngineResult result{true, ResultCode::Accepted, {}, {}, 0, 0, 0};
        const Side opposite = incoming.side == Side::Buy ? Side::Sell : Side::Buy;
        while (incoming.quantity > 0) {
            Resting* resting = best_resting(opposite);
            if (!resting) break;
            const bool crosses = incoming.side == Side::Buy
                ? incoming.price >= resting->order.price
                : incoming.price <= resting->order.price;
            if (!crosses) break;

            const Quantity quantity = std::min(incoming.quantity, resting->order.quantity);
            result.trades.push_back(Trade{next_trade_id_++, incoming.id, resting->order.id,
                                          resting->order.price, quantity, timestamp_ns});
            incoming.quantity -= quantity;
            resting->order.quantity -= quantity;
            if (resting->order.quantity == 0) remove(resting->order.id);
        }

        if (incoming.quantity > 0) resting_.push_back({incoming, next_sequence_++});
        result.resting_quantity = incoming.quantity;
        return result;
    }

    EngineResult cancel(OrderId id) {
        if (!find(id)) return reject(ResultCode::UnknownOrderId);
        remove(id);
        return {true, ResultCode::Accepted, {}, {}, 0, 0, 0};
    }

    EngineResult modify(OrderId id, Price price, Quantity quantity, std::uint64_t timestamp_ns) {
        Resting* resting = find(id);
        if (!resting) return reject(ResultCode::UnknownOrderId);
        if (price <= 0) return reject(ResultCode::InvalidPrice);
        if (quantity == 0) return reject(ResultCode::InvalidQuantity);

        // Same price and no larger: shrink in place and keep time priority.
        if (price == resting->order.price && quantity <= resting->order.quantity) {
            resting->order.quantity = quantity;
            return {true, ResultCode::Accepted, {}, {}, quantity, 0, 0};
        }
        // Otherwise: cancel, then enter as a brand-new order (may match).
        const Side side = resting->order.side;
        remove(id);
        return submit(Order{id, side, price, quantity}, timestamp_ns);
    }

    EngineResult execute(OrderId id, Quantity quantity) {
        Resting* resting = find(id);
        if (!resting) return reject(ResultCode::UnknownOrderId);
        if (quantity == 0) return reject(ResultCode::InvalidQuantity);
        if (quantity > resting->order.quantity) {
            return reject(ResultCode::ExecutionQuantityExceedsRemaining);
        }
        resting->order.quantity -= quantity;
        const Quantity remaining = resting->order.quantity;
        if (remaining == 0) remove(id);
        return {true, ResultCode::Accepted, {}, {}, remaining, 0, 0};
    }

    std::vector<Resting> resting_;
    std::uint64_t next_sequence_{1};
    TradeId next_trade_id_{1};
};

}  // namespace lob::testing
