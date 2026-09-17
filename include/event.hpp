#pragma once

#include "order.hpp"

#include <cstdint>

namespace lob {

enum class EventType { Add, Cancel, Modify, Execute };

struct Event {
    std::uint64_t timestamp_ns;
    EventType type;
    Order order;
    SymbolId symbol_id{1};
    RequestSequence sequence{0};
};

}  // namespace lob
