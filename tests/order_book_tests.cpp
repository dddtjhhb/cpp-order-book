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

    lob::OrderBook matcher;
    check(matcher.add(order(100, lob::Side::Sell, 10025, 5)).accepted, "seed best ask");
    check(matcher.add(order(101, lob::Side::Sell, 10030, 8)).accepted, "seed second ask");
    check(matcher.add(order(102, lob::Side::Sell, 10030, 4)).accepted, "seed FIFO ask");

    const auto buy_sweep = matcher.submit(order(200, lob::Side::Buy, 10030, 10), 5000);
    check(buy_sweep.accepted, "crossing buy accepted");
    check(buy_sweep.message == "fully matched", "crossing buy fully matched");
    check(buy_sweep.trades.size() == 2, "buy sweeps two resting orders");
    check(buy_sweep.trades[0].resting_order_id == 100, "best price executes first");
    check(buy_sweep.trades[0].price == 10025 && buy_sweep.trades[0].quantity == 5,
          "first trade uses resting price and quantity");
    check(buy_sweep.trades[1].resting_order_id == 101,
          "FIFO order executes first at same price");
    check(buy_sweep.trades[1].price == 10030 && buy_sweep.trades[1].quantity == 5,
          "second trade partially fills resting order");
    check(matcher.find_order(101)->quantity == 3, "resting ask retains partial quantity");
    check(matcher.fifo_at(lob::Side::Sell, 10030) == std::vector<lob::OrderId>({101, 102}),
          "partial execution preserves resting FIFO");

    const auto non_crossing_buy = matcher.submit(order(201, lob::Side::Buy, 10020, 7), 5010);
    check(non_crossing_buy.accepted && non_crossing_buy.trades.empty(),
          "non-crossing buy becomes resting order");
    check(non_crossing_buy.resting_quantity == 7, "report resting quantity");
    check(matcher.top().best_bid == 10020, "resting buy updates best bid");

    const auto sell_sweep = matcher.submit(order(300, lob::Side::Sell, 10015, 10), 5020);
    check(sell_sweep.accepted, "crossing sell accepted");
    check(sell_sweep.trades.size() == 1, "sell matches available best bid");
    check(sell_sweep.trades[0].resting_order_id == 201, "sell matches resting buy");
    check(sell_sweep.trades[0].price == 10020, "sell executes at resting price");
    check(sell_sweep.resting_quantity == 3, "unfilled sell remainder rests");
    check(matcher.find_order(300)->quantity == 3, "sell remainder stored in book");
    check(matcher.top().best_bid == std::nullopt, "filled bid level removed");

    const auto duplicate = matcher.submit(order(300, lob::Side::Sell, 10010, 1), 5030);
    check(!duplicate.accepted, "reject duplicate incoming order id");

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
