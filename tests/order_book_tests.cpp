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
    check(book.submit(order(1, lob::Side::Buy, 10000, 10), 0).accepted, "add bid");
    check(book.submit(order(2, lob::Side::Buy, 10005, 5), 0).accepted, "add better bid");
    check(book.submit(order(3, lob::Side::Sell, 10010, 8), 0).accepted, "add ask");
    check(book.top().best_bid == 10005, "best bid is highest");
    check(book.top().best_ask == 10010, "best ask is lowest");
    check(book.top().spread == 5, "spread uses integer ticks");
    check(!book.submit(order(1, lob::Side::Sell, 10020, 1), 0).accepted, "reject duplicate id");
    check(book.submit(order(1, lob::Side::Sell, 10020, 1), 0).code ==
              lob::ResultCode::DuplicateOrderId,
          "duplicate id has stable result code");
    check(!book.submit(order(4, lob::Side::Buy, 10000, 0), 0).accepted, "reject zero quantity");
    check(!book.cancel(999).accepted, "reject unknown cancel");
    check(book.cancel(2).accepted, "cancel existing order");
    check(book.top().best_bid == 10000, "best bid changes after cancel");
    check(book.order_count() == 2, "active order count");

    lob::OrderBook lifecycle;
    check(lifecycle.submit(order(10, lob::Side::Buy, 10000, 10), 0).accepted, "FIFO add first");
    check(lifecycle.submit(order(11, lob::Side::Buy, 10000, 20), 0).accepted, "FIFO add second");
    check(lifecycle.submit(order(12, lob::Side::Buy, 10000, 30), 0).accepted, "FIFO add third");
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
    check(matcher.submit(order(100, lob::Side::Sell, 10025, 5), 0).accepted, "seed best ask");
    check(matcher.submit(order(101, lob::Side::Sell, 10030, 8), 0).accepted, "seed second ask");
    check(matcher.submit(order(102, lob::Side::Sell, 10030, 4), 0).accepted, "seed FIFO ask");

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

    lob::OrderBook process_matcher(7);
    check(process_matcher.submit(order(400, lob::Side::Sell, 10010, 5), 0).accepted,
          "seed process matcher");
    const lob::Event process_add{6000, lob::EventType::Add,
                                 order(401, lob::Side::Buy, 10010, 3), 7, 99};
    const auto process_result = process_matcher.process(process_add);
    check(process_result.accepted && process_result.trades.size() == 1,
          "process preserves add trade reports");
    check(process_result.sequence == 99, "process echoes request sequence");
    check(process_result.trades[0].timestamp_ns == 6000,
          "process forwards event timestamp to trade");

    lob::OrderBook replace_matcher;
    check(replace_matcher.submit(order(500, lob::Side::Buy, 9990, 7), 0).accepted,
          "seed replace bid");
    check(replace_matcher.submit(order(501, lob::Side::Sell, 10000, 4), 0).accepted,
          "seed replace best ask");
    check(replace_matcher.submit(order(502, lob::Side::Sell, 10005, 8), 0).accepted,
          "seed replace second ask");
    const auto crossing_replace = replace_matcher.modify(500, 10005, 10, 7000);
    check(crossing_replace.accepted, "marketable replace accepted");
    check(crossing_replace.trades.size() == 2, "marketable replace sweeps price levels");
    check(crossing_replace.trades[0].resting_order_id == 501 &&
              crossing_replace.trades[1].resting_order_id == 502,
          "marketable replace respects price priority");
    check(crossing_replace.resting_quantity == 0, "fully matched replace does not rest");
    check(!replace_matcher.find_order(500), "fully matched replacement removed from book");
    check(replace_matcher.find_order(502)->quantity == 2,
          "replace leaves partially filled resting order");
    check(crossing_replace.trades[0].timestamp_ns == 7000,
          "replace trades carry request timestamp");

    lob::OrderBook partial_replace_matcher;
    check(partial_replace_matcher.submit(order(600, lob::Side::Buy, 9990, 10), 0).accepted,
          "seed partial replace bid");
    check(partial_replace_matcher.submit(order(601, lob::Side::Sell, 10000, 4), 0).accepted,
          "seed partial replace ask");
    const auto partial_replace = partial_replace_matcher.modify(600, 10000, 10, 8000);
    check(partial_replace.accepted && partial_replace.trades.size() == 1,
          "partially matched replace emits trade");
    check(partial_replace.resting_quantity == 6,
          "partially matched replace reports resting quantity");
    check(partial_replace_matcher.find_order(600)->quantity == 6,
          "replacement remainder rests in book");

    const auto before_invalid_replace = partial_replace_matcher.find_order(600);
    const auto invalid_replace = partial_replace_matcher.modify(600, 0, 3, 9000);
    check(!invalid_replace.accepted && invalid_replace.code == lob::ResultCode::InvalidPrice,
          "invalid replace has stable result code");
    check(partial_replace_matcher.find_order(600)->price == before_invalid_replace->price &&
              partial_replace_matcher.find_order(600)->quantity == before_invalid_replace->quantity,
          "invalid replace preserves original order");

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
    check(events[0].symbol_id == 1 && events[0].sequence == 1,
          "legacy CSV defaults symbol and sequence");

    std::istringstream extended_csv(
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity,symbol_id,sequence\n"
        "10,ADD,20,BUY,10000,5,7,123\n");
    const auto extended_events = lob::read_events(extended_csv);
    check(extended_events.size() == 1, "parse extended CSV event");
    check(extended_events[0].symbol_id == 7, "parse symbol id");
    check(extended_events[0].sequence == 123, "parse request sequence");

    if (failures == 0) {
        std::cout << "All order-book tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
