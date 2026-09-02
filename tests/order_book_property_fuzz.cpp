#include "order_book.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class OperationType { Submit, Cancel, Modify, Execute };

struct Operation {
    OperationType type;
    lob::OrderId id;
    lob::Side side;
    lob::Price price;
    lob::Quantity quantity;
};

struct Failure {
    std::size_t step;
    std::string message;
};

std::string operation_name(OperationType type) {
    switch (type) {
        case OperationType::Submit: return "SUBMIT";
        case OperationType::Cancel: return "CANCEL";
        case OperationType::Modify: return "MODIFY";
        case OperationType::Execute: return "EXECUTE";
    }
    return "UNKNOWN";
}

std::string describe(const Operation& operation) {
    std::ostringstream out;
    out << operation_name(operation.type) << ',' << operation.id << ','
        << lob::to_string(operation.side) << ',' << operation.price << ','
        << operation.quantity;
    return out.str();
}

std::vector<lob::OrderId> active_ids(
    const lob::OrderBook& book, const std::vector<lob::OrderId>& known_ids) {
    std::vector<lob::OrderId> active;
    for (const auto id : known_ids) {
        if (book.find_order(id)) active.push_back(id);
    }
    return active;
}

std::optional<Failure> replay(const std::vector<Operation>& operations) {
    lob::OrderBook book;
    for (std::size_t step = 0; step < operations.size(); ++step) {
        const auto& operation = operations[step];
        const auto existing = book.find_order(operation.id);

        if (operation.type == OperationType::Submit) {
            const auto before = book.top();
            std::optional<lob::OrderId> expected_first_resting;
            if (operation.side == lob::Side::Buy && before.best_ask &&
                operation.price >= *before.best_ask) {
                const auto fifo = book.fifo_at(lob::Side::Sell, *before.best_ask);
                if (!fifo.empty()) expected_first_resting = fifo.front();
            } else if (operation.side == lob::Side::Sell && before.best_bid &&
                       operation.price <= *before.best_bid) {
                const auto fifo = book.fifo_at(lob::Side::Buy, *before.best_bid);
                if (!fifo.empty()) expected_first_resting = fifo.front();
            }

            const auto result = book.submit(
                {operation.id, operation.side, operation.price, operation.quantity}, step + 1);
            if (!result.accepted) return Failure{step, "valid unique submit was rejected"};
            lob::Quantity traded = 0;
            for (const auto& trade : result.trades) {
                if (trade.quantity == 0) return Failure{step, "zero-quantity trade"};
                traded += trade.quantity;
            }
            if (traded + result.resting_quantity != operation.quantity) {
                return Failure{step, "submit quantity was not conserved"};
            }
            if (expected_first_resting &&
                (result.trades.empty() ||
                 result.trades.front().resting_order_id != *expected_first_resting)) {
                return Failure{step, "first trade violated price-time priority"};
            }
        } else if (operation.type == OperationType::Cancel) {
            const auto result = book.cancel(operation.id);
            if (result.accepted != existing.has_value()) {
                return Failure{step, "cancel acceptance disagrees with order existence"};
            }
        } else if (operation.type == OperationType::Execute) {
            const auto result = book.execute(operation.id, operation.quantity);
            const bool should_accept = existing && operation.quantity > 0 &&
                operation.quantity <= existing->quantity;
            if (result.accepted != should_accept) {
                return Failure{step, "execute validation disagrees with model"};
            }
        } else {
            std::vector<lob::OrderId> before_fifo;
            if (existing) before_fifo = book.fifo_at(existing->side, existing->price);
            const auto result = book.modify(operation.id, operation.price, operation.quantity);
            const bool should_accept = existing && operation.price > 0 && operation.quantity > 0;
            if (result.accepted != should_accept) {
                return Failure{step, "modify validation disagrees with model"};
            }
            if (result.accepted && operation.price == existing->price &&
                operation.quantity <= existing->quantity &&
                book.fifo_at(existing->side, existing->price) != before_fifo) {
                return Failure{step, "priority-preserving modify changed FIFO order"};
            }
            if (result.accepted && (operation.price != existing->price ||
                                    operation.quantity > existing->quantity)) {
                const auto after_fifo = book.fifo_at(existing->side, operation.price);
                if (after_fifo.empty() || after_fifo.back() != operation.id) {
                    return Failure{step, "priority-resetting modify did not move order to FIFO back"};
                }
            }
        }

        if (const auto invariant = book.validate_invariants()) {
            return Failure{step, *invariant};
        }
    }
    return std::nullopt;
}

std::vector<Operation> minimize(std::vector<Operation> operations) {
    std::size_t chunk = std::max<std::size_t>(1, operations.size() / 2);
    while (chunk >= 1) {
        bool reduced = false;
        for (std::size_t start = 0; start + chunk <= operations.size(); ++start) {
            std::vector<Operation> candidate;
            candidate.reserve(operations.size() - chunk);
            candidate.insert(candidate.end(), operations.begin(), operations.begin() + start);
            candidate.insert(candidate.end(), operations.begin() + start + chunk, operations.end());
            if (!candidate.empty() && replay(candidate)) {
                operations = std::move(candidate);
                reduced = true;
                break;
            }
        }
        if (!reduced) {
            if (chunk == 1) break;
            chunk = std::max<std::size_t>(1, chunk / 2);
        } else {
            chunk = std::min(chunk, std::max<std::size_t>(1, operations.size() / 2));
        }
    }
    return operations;
}

Operation generate_operation(std::mt19937_64& random, lob::OrderBook& model_book,
                             std::vector<lob::OrderId>& known_ids, lob::OrderId& next_id) {
    const auto active = active_ids(model_book, known_ids);
    const int choice = static_cast<int>(random() % 100);
    if (active.empty() || choice < 45) {
        const auto side = random() % 2 == 0 ? lob::Side::Buy : lob::Side::Sell;
        const lob::Price price = 9980 + static_cast<lob::Price>(random() % 41);
        const lob::Quantity quantity = 1 + random() % 100;
        const auto id = next_id++;
        known_ids.push_back(id);
        Operation operation{OperationType::Submit, id, side, price, quantity};
        model_book.submit({id, side, price, quantity}, next_id);
        return operation;
    }

    const auto id = active[random() % active.size()];
    const auto stored = *model_book.find_order(id);
    if (choice < 65) {
        Operation operation{OperationType::Cancel, id, stored.side, stored.price, stored.quantity};
        model_book.cancel(id);
        return operation;
    }
    if (choice < 85) {
        lob::Price new_price = 9980 + static_cast<lob::Price>(random() % 41);
        const auto top = model_book.top();
        if (stored.side == lob::Side::Buy && top.best_ask) {
            new_price = std::min(new_price, *top.best_ask - 1);
        } else if (stored.side == lob::Side::Sell && top.best_bid) {
            new_price = std::max(new_price, *top.best_bid + 1);
        }
        const lob::Quantity new_quantity = random() % 4 == 0
            ? stored.quantity
            : 1 + random() % 120;
        Operation operation{OperationType::Modify, id, stored.side, new_price, new_quantity};
        model_book.modify(id, new_price, new_quantity);
        return operation;
    }

    const lob::Quantity executed = random() % 5 == 0
        ? stored.quantity + 1 + random() % 10
        : 1 + random() % stored.quantity;
    Operation operation{OperationType::Execute, id, stored.side, stored.price, executed};
    model_book.execute(id, executed);
    return operation;
}

void save_failure(const std::string& path, std::uint64_t seed,
                  const std::vector<Operation>& operations, const Failure& failure) {
    std::ofstream output(path);
    output << "# seed=" << seed << "\n# failure_step=" << failure.step
           << "\n# reason=" << failure.message << "\n";
    for (const auto& operation : operations) output << describe(operation) << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t seed = argc > 1 ? std::stoull(argv[1]) : 1;
    const std::size_t steps = argc > 2 ? std::stoull(argv[2]) : 10000;
    const std::string failure_path = argc > 3 ? argv[3] : "fuzz_failure.txt";
    if (steps == 0) return EXIT_FAILURE;

    std::mt19937_64 random(seed);
    lob::OrderBook generation_model;
    std::vector<lob::OrderId> known_ids;
    std::vector<Operation> operations;
    operations.reserve(steps);
    lob::OrderId next_id = 1;
    for (std::size_t i = 0; i < steps; ++i) {
        operations.push_back(generate_operation(random, generation_model, known_ids, next_id));
    }

    if (const auto failure = replay(operations)) {
        const auto minimized = minimize(operations);
        const auto minimized_failure = *replay(minimized);
        save_failure(failure_path, seed, minimized, minimized_failure);
        std::cerr << "property failure: " << failure->message << " at step " << failure->step
                  << "; minimized from " << operations.size() << " to " << minimized.size()
                  << " operations; saved to " << failure_path << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "property_fuzz_passed seed=" << seed << " steps=" << steps << '\n';
}
