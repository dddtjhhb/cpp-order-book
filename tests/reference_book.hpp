#pragma once

#include "order_book.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <locale>

// Deliberately slow oracle: one vector in arrival order and linear scans for
// best price/FIFO. It shares data types, but no matching/container code.
class ReferenceBook {
public:
    explicit ReferenceBook(lob::SymbolId symbol = 1) : symbol_(symbol) {}

    lob::EngineResult process(const lob::Event& event) {
        using namespace lob;
        EngineResult output{false, ResultCode::Accepted, {}, {}, 0, event.sequence,
                            event.symbol_id};
        const auto reject = [&](ResultCode reason) {
            output.code = reason;
            return output;
        };
        if (event.symbol_id != symbol_) return reject(ResultCode::UnknownSymbol);
        auto found = std::find_if(orders.begin(), orders.end(), [&](const Order& order) {
            return order.id == event.order.id;
        });
        const auto& input = event.order;
        if (event.type == EventType::Cancel || event.type == EventType::Execute) {
            if (found == orders.end()) return reject(ResultCode::UnknownOrderId);
            if (event.type == EventType::Cancel) {
                orders.erase(found);
            } else {
                if (input.quantity == 0) return reject(ResultCode::InvalidQuantity);
                if (input.quantity > found->quantity) return reject(ResultCode::ExecutionQuantityExceedsRemaining);
                found->quantity -= input.quantity;
                output.resting_quantity = found->quantity;
                if (found->quantity == 0) orders.erase(found);
            }
            output.accepted = true;
            return output;
        }
        Order incoming = input;
        if (event.type == EventType::Add) {
            if (input.side != Side::Buy && input.side != Side::Sell) return reject(ResultCode::InvalidSide);
            if (input.quantity == 0) return reject(ResultCode::InvalidQuantity);
            if (input.price <= 0) return reject(ResultCode::InvalidPrice);
            if (found != orders.end()) return reject(ResultCode::DuplicateOrderId);
        } else if (event.type == EventType::Modify) {
            if (found == orders.end()) return reject(ResultCode::UnknownOrderId);
            if (input.price <= 0) return reject(ResultCode::InvalidPrice);
            if (input.quantity == 0) return reject(ResultCode::InvalidQuantity);
            incoming.side = found->side;
        } else {
            return reject(ResultCode::UnsupportedEventType);
        }
        Quantity available = std::numeric_limits<Quantity>::max();
        for (const auto& order : orders) {
            if (order.id != incoming.id && order.side == incoming.side && order.price == incoming.price) {
                available -= order.quantity;
            }
        }
        if (incoming.quantity > available) return reject(ResultCode::QuantityOverflow);
        if (event.type == EventType::Modify) {
            if (input.price == found->price && input.quantity <= found->quantity) {
                found->quantity = input.quantity;
                output.accepted = true;
                output.resting_quantity = input.quantity;
                return output;
            }
            orders.erase(found);
        }
        while (incoming.quantity != 0) {
            auto best = orders.end();
            for (auto candidate = orders.begin(); candidate != orders.end(); ++candidate) {
                if (candidate->side == incoming.side) continue;
                if (incoming.side == Side::Buy ? candidate->price > incoming.price
                                               : candidate->price < incoming.price) continue;
                if (best == orders.end() || (incoming.side == Side::Buy
                    ? candidate->price < best->price : candidate->price > best->price)) best = candidate;
            }
            if (best == orders.end()) break;
            const auto quantity = std::min(incoming.quantity, best->quantity);
            output.trades.push_back({next_trade_++, incoming.id, best->id, best->price,
                                     quantity, event.timestamp_ns});
            incoming.quantity -= quantity;
            best->quantity -= quantity;
            if (best->quantity == 0) orders.erase(best);
        }
        output.accepted = true;
        output.resting_quantity = incoming.quantity;
        if (incoming.quantity != 0) orders.push_back(incoming);
        return output;
    }

    std::vector<lob::Order> orders;
private:
    lob::SymbolId symbol_;
    lob::TradeId next_trade_{1};
};

// Canonical logical output for comparisons, never native struct bytes/padding.
// Diagnostic text is deliberately outside the stable contract.
inline std::string logical_output(const lob::EngineResult& result) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << result.accepted << ',' << static_cast<unsigned>(result.code) << ','
        << result.symbol_id << ',' << result.sequence << ','
        << result.resting_quantity << ',' << result.trades.size() << '\n';
    for (const auto& trade : result.trades) {
        out << trade.id << ',' << trade.incoming_order_id << ',' << trade.resting_order_id
            << ',' << trade.price << ',' << trade.quantity << ',' << trade.timestamp_ns << '\n';
    }
    return out.str();
}

inline bool same_state(const lob::OrderBook& book, const ReferenceBook& model) {
    if (book.order_count() != model.orders.size()) return false;
    std::optional<lob::Price> bid, ask;
    for (const auto& expected : model.orders) {
        const auto actual = book.find_order(expected.id);
        if (!actual || actual->side != expected.side || actual->price != expected.price ||
            actual->quantity != expected.quantity) return false;
        std::vector<lob::OrderId> fifo;
        for (const auto& order : model.orders) {
            if (order.side == expected.side && order.price == expected.price) fifo.push_back(order.id);
        }
        if (book.fifo_at(expected.side, expected.price) != fifo) return false;
        if (expected.side == lob::Side::Buy && (!bid || expected.price > *bid)) bid = expected.price;
        if (expected.side == lob::Side::Sell && (!ask || expected.price < *ask)) ask = expected.price;
    }
    const auto top = book.top();
    return top.best_bid == bid && top.best_ask == ask &&
        top.spread == (bid && ask ? std::optional<lob::Price>(*ask - *bid) : std::nullopt);
}
