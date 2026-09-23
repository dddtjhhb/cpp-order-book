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

bool csv_rejects(const std::string& row) {
    std::istringstream input("timestamp_ns,event_type,order_id,side,price_ticks,quantity\n" + row);
    try {
        lob::read_events(input);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
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

    // Regression (v0.6): a MODIFY that moved a bid through the best ask left a
    // crossed resting book (best_bid 110 > best_ask 105) with no trade.
    lob::OrderBook modify_cross;
    check(modify_cross.add(order(1, lob::Side::Buy, 100, 10)).accepted, "seed modify bid");
    check(modify_cross.add(order(2, lob::Side::Sell, 105, 4)).accepted, "seed modify ask");
    check(modify_cross.add(order(3, lob::Side::Sell, 107, 3)).accepted, "seed second modify ask");
    const auto crossing_modify = modify_cross.modify(1, 106, 10, 9000);
    check(crossing_modify.accepted, "marketable modify accepted");
    check(crossing_modify.trades.size() == 1, "marketable modify trades against best ask");
    check(crossing_modify.trades[0].incoming_order_id == 1 &&
              crossing_modify.trades[0].resting_order_id == 2,
          "modified order is the aggressor");
    check(crossing_modify.trades[0].price == 105 && crossing_modify.trades[0].quantity == 4,
          "modify trade uses resting price and quantity");
    check(crossing_modify.trades[0].timestamp_ns == 9000, "modify trade carries event time");
    check(crossing_modify.resting_quantity == 6, "modify remainder rests");
    check(modify_cross.find_order(1)->price == 106 && modify_cross.find_order(1)->quantity == 6,
          "modify remainder rests at new limit");
    check(modify_cross.top().best_bid == 106 && modify_cross.top().best_ask == 107,
          "book is uncrossed after marketable modify");
    check(!modify_cross.validate_invariants().has_value(), "marketable modify keeps invariants");

    const auto sweeping_modify = modify_cross.modify(1, 107, 3, 9010);
    check(sweeping_modify.accepted && sweeping_modify.resting_quantity == 0,
          "modify can fully fill");
    check(!modify_cross.find_order(1).has_value(), "fully filled modify leaves no order");
    check(modify_cross.order_count() == 0, "fully filled modify empties book");

    lob::OrderBook replay_cross;
    const auto replayed = replay_cross.process(
        {9020, lob::EventType::Add, order(10, lob::Side::Sell, 200, 5)});
    check(replayed.accepted, "process add accepted");
    replay_cross.process({9030, lob::EventType::Add, order(11, lob::Side::Buy, 190, 5)});
    const auto replay_modify = replay_cross.process(
        {9040, lob::EventType::Modify, order(11, lob::Side::Buy, 200, 5)});
    check(replay_modify.trades.size() == 1, "process() reports trades from MODIFY");
    check(!replay_cross.validate_invariants().has_value(), "replayed modify keeps invariants");

    // Regression (v0.6): std::stoull wrapped "-5" to 2^64 - 5 and the order was
    // accepted with 18446744073709551611 shares.
    check(csv_rejects("1,ADD,1,BUY,100,-5\n"), "CSV rejects negative quantity");
    check(csv_rejects("1,ADD,-1,BUY,100,5\n"), "CSV rejects negative order id");
    check(csv_rejects("-1,ADD,1,BUY,100,5\n"), "CSV rejects negative timestamp");
    check(csv_rejects("1,ADD,1,BUY,100,18446744073709551616\n"), "CSV rejects quantity overflow");
    check(csv_rejects("1,ADD,1,BUY,9223372036854775808,5\n"), "CSV rejects price overflow");
    check(csv_rejects("1,ADD,1,BUY,100,5x\n"), "CSV rejects trailing garbage");
    check(csv_rejects("1,ADD,1,BUY,100, 5\n"), "CSV rejects embedded whitespace");
    check(csv_rejects("1,ADD,1,BUY,100,\n"), "CSV rejects empty field");
    check(csv_rejects("1,ADD,1,BUY,100,5,extra\n"), "CSV rejects extra column");
    check(csv_rejects("1,ADD,1,BUY,100\n"), "CSV rejects missing column");
    check(csv_rejects("1,ADD,1,HOLD,100,5\n"), "CSV rejects unknown side");

    std::istringstream crlf(
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity\r\n"
        "1,ADD,7,SELL,-3,5\r\n");
    const auto crlf_events = lob::read_events(crlf);
    check(crlf_events.size() == 1 && crlf_events[0].order.quantity == 5,
          "CSV accepts CRLF line endings");
    check(crlf_events[0].order.price == -3,
          "CSV parses signed price; the book, not the parser, rejects it");

    if (failures == 0) {
        std::cout << "All order-book tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
