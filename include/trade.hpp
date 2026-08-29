#pragma once

#include "order.hpp"

#include <cstdint>

namespace lob {

using TradeId = std::uint64_t;

struct Trade {
    TradeId id;
    OrderId incoming_order_id;
    OrderId resting_order_id;
    Price price;
    Quantity quantity;
    std::uint64_t timestamp_ns;
};

}  // namespace lob
