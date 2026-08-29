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

    lob::OrderBook lifecycle;
    check(lifecycle.add(order(10, lob::Side::Buy, 10000, 10)).accepted, "FIFO add first");
    check(lifecycle.add(order(11, lob::Side::Buy, 10000, 20)).accepted, "FIFO add second");
    check(lifecycle.add(order(12, lob::Side::Buy, 10000, 30)).accepted, "FIFO add third");
    check(lifecycle.fifo_at(lob::Side::Buy, 10000) == std::vector<lob::OrderId>({10, 11, 12}),
          "same-price orders preserve FIFO");

    check(lifecycle.execute(10, 4).accepted, "partial execution accepted");
    check(lifecycle.find_order(10)->quantity == 6, "partial execution reduces remaining quantity");
    check(lifecycle.fifo_at(lob::Side::Buy, 10000).front() == 10,
          "partial execution preserves priority");
    check(!lifecycle.execute(10, 7).accepted, "reject over-execution");
    check(lifecycle.execute(10, 6).accepted, "full execution accepted");
    check(!lifecycle.find_order(10).has_value(), "full execution removes order");
    check(lifecycle.fifo_at(lob::Side::Buy, 10000) == std::vector<lob::OrderId>({11, 12}),
          "full execution removes FIFO head");

    check(lifecycle.modify(11, 10000, 15).accepted, "quantity decrease accepted");
    check(lifecycle.fifo_at(lob::Side::Buy, 10000).front() == 11,
          "quantity decrease preserves priority");
    check(lifecycle.modify(11, 10000, 25).accepted, "quantity increase accepted");
    check(lifecycle.fifo_at(lob::Side::Buy, 10000) == std::vector<lob::OrderId>({12, 11}),
          "quantity increase resets priority");
    check(lifecycle.modify(12, 10005, 30).accepted, "price change accepted");
    check(lifecycle.top().best_bid == 10005, "price modification updates best bid");
    check(lifecycle.fifo_at(lob::Side::Buy, 10005) == std::vector<lob::OrderId>({12}),
          "price modification moves order to new level");
    check(!lifecycle.modify(999, 10000, 1).accepted, "reject modify of unknown order");
    check(!lifecycle.execute(999, 1).accepted, "reject execution of unknown order");

    std::istringstream csv(
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity\n"
        "1,ADD,10,BUY,9990,7\n"
        "2,MODIFY,10,BUY,9991,6\n"
        "3,EXECUTE,10,BUY,9991,2\n"
        "4,CANCEL,10,BUY,9991,4\n");
    const auto events = lob::read_events(csv);
    check(events.size() == 4, "parse four CSV events");
    check(events[0].order.id == 10, "parse order id");
    check(events[1].type == lob::EventType::Modify, "parse modify type");
    check(events[2].type == lob::EventType::Execute, "parse execute type");
    check(events[3].type == lob::EventType::Cancel, "parse cancel type");

    if (failures == 0) {
        std::cout << "All order-book tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
