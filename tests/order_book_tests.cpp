#include "csv_reader.hpp"
#include "order_book.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

lob::Order order(lob::OrderId id, lob::Side side, lob::Price price, lob::Quantity quantity) {
    return {id, side, price, quantity};
}

}  // namespace

int main() {
    lob::OrderBook book;
    check(book.add(order(1, lob::Side::Buy, 10000, 10)).accepted, "add bid");
    check(book.add(order(2, lob::Side::Buy, 10005, 5)).accepted, "add better bid");
    check(book.add(order(3, lob::Side::Sell, 10010, 8)).accepted, "add ask");
    check(book.top().best_bid == 10005, "best bid is highest");
    check(book.top().best_ask == 10010, "best ask is lowest");
    check(book.top().spread == 5, "spread uses integer ticks");
    check(!book.add(order(1, lob::Side::Sell, 10020, 1)).accepted, "reject duplicate id");
    check(!book.add(order(4, lob::Side::Buy, 10000, 0)).accepted, "reject zero quantity");
    check(!book.cancel(999).accepted, "reject unknown cancel");
    check(book.cancel(2).accepted, "cancel existing order");
    check(book.top().best_bid == 10000, "best bid changes after cancel");
    check(book.order_count() == 2, "active order count");

    std::istringstream csv(
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity\n"
        "1,ADD,10,BUY,9990,7\n"
        "2,CANCEL,10,BUY,9990,7\n");
    const auto events = lob::read_events(csv);
    check(events.size() == 2, "parse two CSV events");
    check(events[0].order.id == 10, "parse order id");
    check(events[1].type == lob::EventType::Cancel, "parse cancel type");

    if (failures == 0) {
        std::cout << "All order-book tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
