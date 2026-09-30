#include "csv_reader.hpp"
#include "reference_book.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
int failures = 0;
void check(bool condition, const char* name) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

lob::Event event(lob::EventType type, lob::OrderId id, lob::Side side,
                 lob::Price price, lob::Quantity quantity,
                 lob::RequestSequence sequence = 1, lob::SymbolId symbol = 1) {
    return {1234, type, {id, side, price, quantity}, symbol, sequence};
}

void rejection_table() {
    using namespace lob;
    struct Case { Event input; ResultCode code; };
    const Case cases[] = {
        {event(EventType::Add, 2, Side::Buy, 100, 0), ResultCode::InvalidQuantity},
        {event(EventType::Add, 2, Side::Buy, 0, 1), ResultCode::InvalidPrice},
        {event(EventType::Add, 2, Side::Buy, -1, 1), ResultCode::InvalidPrice},
        {event(EventType::Add, 1, Side::Buy, 100, 1), ResultCode::DuplicateOrderId},
        {event(EventType::Add, 2, static_cast<Side>(99), 100, 1), ResultCode::InvalidSide},
        {event(EventType::Cancel, 999, Side::Buy, 100, 1), ResultCode::UnknownOrderId},
        {event(EventType::Modify, 999, Side::Buy, 100, 1), ResultCode::UnknownOrderId},
        {event(EventType::Execute, 999, Side::Buy, 100, 1), ResultCode::UnknownOrderId},
        {event(EventType::Modify, 1, Side::Buy, 0, 1), ResultCode::InvalidPrice},
        {event(EventType::Modify, 1, Side::Buy, 101, 0), ResultCode::InvalidQuantity},
        {event(EventType::Execute, 1, Side::Buy, 100, 0), ResultCode::InvalidQuantity},
        {event(EventType::Execute, 1, Side::Buy, 100, 11), ResultCode::ExecutionQuantityExceedsRemaining},
        {event(static_cast<EventType>(99), 1, Side::Buy, 100, 1), ResultCode::UnsupportedEventType},
        {event(EventType::Cancel, 1, Side::Buy, 100, 1, 777, 2), ResultCode::UnknownSymbol},
    };
    for (const auto& test : cases) {
        OrderBook book;
        ReferenceBook reference;
        const auto seed = event(EventType::Add, 1, Side::Buy, 100, 10);
        book.process(seed);
        reference.process(seed);
        const auto result = book.process(test.input);
        check(!result.accepted && result.code == test.code, "stable rejection code");
        check(result.trades.empty() && result.resting_quantity == 0, "rejection has no executions");
        check(result.symbol_id == test.input.symbol_id &&
              result.sequence == test.input.sequence, "rejection correlation");
        check(same_state(book, reference) && !book.validate_invariants(), "rejection leaves state unchanged");
    }
}

void matching_and_priority() {
    using namespace lob;
    for (const auto side : {Side::Buy, Side::Sell}) {
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        const Price initial = side == Side::Buy ? 90 : 110;
        const Price next_level = side == Side::Buy ? 101 : 99;
        OrderBook book;
        book.process(event(EventType::Add, 1, side, initial, 10));
        book.process(event(EventType::Add, 2, opposite, 100, 2));
        book.process(event(EventType::Add, 3, opposite, 100, 3));
        book.process(event(EventType::Add, 4, opposite, next_level, 4));
        const auto result = book.process(event(EventType::Modify, 1, opposite, next_level, 12, 77));
        check(result.accepted && result.code == ResultCode::Accepted && result.trades.size() == 3,
              "marketable replacement sweeps levels on both sides");
        check(result.resting_quantity == 3 && result.sequence == 77 && result.symbol_id == 1,
              "replacement returns correlated remaining quantity");
        if (result.trades.size() == 3) {
            for (std::size_t i = 0; i < 3; ++i) {
                const auto& trade = result.trades[i];
                check(trade.id == i + 1 && trade.incoming_order_id == 1 &&
                      trade.resting_order_id == i + 2 && trade.timestamp_ns == 1234,
                      "trade IDs, FIFO and request timestamp");
            }
            check(result.trades[0].price == 100 && result.trades[1].price == 100 &&
                  result.trades[2].price == next_level, "resting-price executions");
            check(result.trades[0].quantity == 2 && result.trades[1].quantity == 3 &&
                  result.trades[2].quantity == 4, "replacement conserves quantity");
        }
        check(book.find_order(1) && book.find_order(1)->side == side &&
              book.find_order(1)->price == next_level && book.find_order(1)->quantity == 3,
              "modify keeps stored side and replaces remaining quantity");
        check(!book.validate_invariants(), "marketable replacement never leaves crossed book");
        const auto fill = book.process(event(EventType::Add, 5, opposite, next_level, 3, 78));
        check(fill.accepted && fill.trades.size() == 1 && fill.resting_quantity == 0 &&
              book.order_count() == 0, "process ADD exposes full execution");
        book.process(event(EventType::Add, 1, side, initial, 1));
        check(book.order_count() == 1, "completed order ID can be reused");
    }
    OrderBook book;
    book.submit({1, Side::Buy, 100, 10}, 0);
    book.submit({2, Side::Buy, 100, 10}, 0);
    check(book.modify(1, 100, 10).resting_quantity == 10 &&
          book.fifo_at(Side::Buy, 100) == std::vector<OrderId>({1, 2}), "equal quantity preserves FIFO");
    check(book.modify(1, 100, 5).resting_quantity == 5 &&
          book.fifo_at(Side::Buy, 100) == std::vector<OrderId>({1, 2}), "quantity reduction preserves FIFO");
    book.modify(1, 100, 6);
    check(book.fifo_at(Side::Buy, 100) == std::vector<OrderId>({2, 1}), "increase resets FIFO");
    check(book.execute(2, 4).resting_quantity == 6 && !book.validate_invariants(),
          "EXECUTE returns remainder and updates totals");
    check(book.cancel(2).resting_quantity == 0 && !book.validate_invariants(), "CANCEL updates totals");
    OrderBook full;
    full.submit({1, Side::Buy, 90, 10}, 0);
    full.submit({2, Side::Sell, 100, 10}, 0);
    const auto replaced = full.modify(1, 100, 10, 99);
    check(replaced.accepted && replaced.resting_quantity == 0 && replaced.trades.size() == 1 &&
          full.order_count() == 0, "fully filled replacement removes original");
}

void overflow_and_atomicity() {
    using namespace lob;
    const auto max = std::numeric_limits<Quantity>::max();
    OrderBook book;
    book.submit({1, Side::Buy, 100, max - 1}, 0);
    book.submit({2, Side::Buy, 100, 1}, 0);
    book.submit({3, Side::Buy, 99, 2}, 0);
    check(book.submit({4, Side::Buy, 100, 1}, 0).code == ResultCode::QuantityOverflow,
          "reject aggregate overflow");
    check(book.modify(2, 100, 2).code == ResultCode::QuantityOverflow,
          "reject same-price increase overflow");
    check(book.modify(3, 100, 2).code == ResultCode::QuantityOverflow &&
          book.find_order(3)->price == 99 && book.find_order(3)->quantity == 2,
          "rejected replacement retains original order");
    check(book.fifo_at(Side::Buy, 100) == std::vector<OrderId>({1, 2}) &&
          !book.validate_invariants(), "overflow rejection preserves FIFO and aggregate");
    check(book.modify(1, 100, max - 1).accepted, "replacement subtracts old quantity before capacity check");
    const auto fill = book.submit({5, Side::Sell, 100, max}, 0);
    check(fill.accepted && fill.trades.size() == 2 && fill.resting_quantity == 0 &&
          !book.validate_invariants(), "maximum quantity can trade without overflow");
}

void deterministic_contract() {
    using namespace lob;
    OrderBook first(7), second(7);
    ReferenceBook reference(7);
    const std::vector<Event> inputs = {
        event(EventType::Add, 1, Side::Sell, 101, 3, 1, 7),
        event(EventType::Add, 2, Side::Sell, 102, 6, 2, 7),
        event(EventType::Add, 3, Side::Buy, 100, 9, 3, 7),
        event(EventType::Modify, 3, Side::Sell, 102, 7, 4, 7),
        event(EventType::Execute, 2, Side::Buy, 0, 1, 5, 7),
        event(EventType::Cancel, 2, Side::Buy, 0, 0, 6, 7),
        event(EventType::Cancel, 2, Side::Buy, 0, 0, 6, 7),
    };
    std::string a, b;
    for (const auto& input : inputs) {
        const auto output = first.process(input);
        a += logical_output(output);
        b += logical_output(second.process(input));
        check(logical_output(output) == logical_output(reference.process(input)), "logical output matches oracle");
        check(same_state(first, reference) && !first.validate_invariants(), "state matches oracle after each command");
    }
    check(a == b, "same input has byte-identical canonical logical output");
    // Client sequence numbers are opaque correlation data, not deduplication.
    check(first.process(event(EventType::Add, 9, Side::Buy, 90, 1, 6, 7)).accepted,
          "duplicate request sequence does not imply duplicate request");
    static_assert(!std::is_copy_constructible_v<OrderBook>);
    static_assert(!std::is_copy_assignable_v<OrderBook>);
    OrderBook moved(std::move(first));
    check(moved.cancel(9).accepted && !moved.validate_invariants(), "move preserves stored FIFO iterators");
}

void csv_validation() {
    const char* invalid[] = {
        "1,ADD,1,BUY,100abc,1\n", "1,ADD,1,BUY,100,-1\n",
        "-1,ADD,1,BUY,100,1\n", "1,ADD,-1,BUY,100,1\n",
        "1,ADD,1,BUY,9223372036854775808,1\n",
        "1,ADD,1,BUY,100,18446744073709551616\n",
        "1,ADD,1,BUY,100,1,4294967296,1\n",
        "1,ADD,1,BUY,100,1,1,-1\n", "1,ADD,1,BUY,100,1,1,18446744073709551616\n",
        "1,ADD,1,BUY,100,\n", "1,ADD,1,BUY,100,1,\n",
        "1,ADD,1,BUY,100,1,1,2,extra\n", "1,ADD,1,BUY,100, 1\n",
        "1,ADD,1,BUY,100,1\n2,ADD,2,BUY,100,1,1,2\n",
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity\n1,ADD,1,BUY,100,1,1,2\n",
        "1,BOGUS,1,BUY,100,1\n", "1,ADD,1,BOGUS,100,1\n",
    };
    for (const auto text : invalid) {
        std::istringstream input(text);
        bool rejected = false;
        try { lob::read_events(input); }
        catch (const std::runtime_error& e) { rejected = std::string(e.what()).find("CSV line ") == 0; }
        check(rejected, "malformed CSV is rejected with line number");
    }
    std::istringstream legacy("1,ADD,1,BUY,100,1\r\n\r\n2,CANCEL,1,BUY,100,1\r\n");
    const auto old = lob::read_events(legacy);
    check(old.size() == 2 && old[0].symbol_id == 1 && old[0].sequence == 1 &&
          old[1].sequence == 2, "legacy CSV gets deterministic sequence and supports CRLF");
    std::istringstream extended(
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity,symbol_id,sequence\n"
        "1,ADD,1,BUY,100,18446744073709551615,7,18446744073709551615\n");
    const auto parsed = lob::read_events(extended);
    check(parsed.size() == 1 && parsed[0].symbol_id == 7 &&
          parsed[0].sequence == std::numeric_limits<lob::RequestSequence>::max() &&
          parsed[0].order.quantity == std::numeric_limits<lob::Quantity>::max(), "exact unsigned maximum parses");
}
}  // namespace

int main() {
    rejection_table();
    matching_and_priority();
    overflow_and_atomicity();
    deterministic_contract();
    csv_validation();
    if (failures == 0) std::cout << "All engine-contract tests passed.\n";
    return failures == 0 ? 0 : 1;
}
