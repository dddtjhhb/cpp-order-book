#pragma once

#include <cstdint>
#include <string>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // Price in integer ticks (e.g. cents).
using Quantity = std::uint64_t;

enum class Side { Buy, Sell };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
};

inline std::string to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

}  // namespace lob
