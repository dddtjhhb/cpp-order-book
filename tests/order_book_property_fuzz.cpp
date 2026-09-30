#include "order_book.hpp"
#include "reference_book.hpp"

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

lob::Event as_event(const Operation& operation, std::uint64_t sequence) {
    lob::EventType type = lob::EventType::Add;
    switch (operation.type) {
        case OperationType::Submit: type = lob::EventType::Add; break;
        case OperationType::Cancel: type = lob::EventType::Cancel; break;
        case OperationType::Modify: type = lob::EventType::Modify; break;
        case OperationType::Execute: type = lob::EventType::Execute; break;
    }
    return {sequence, type, {operation.id, operation.side, operation.price, operation.quantity},
            1, sequence};
}

std::optional<Failure> replay(const std::vector<Operation>& operations) {
    lob::OrderBook book;
    ReferenceBook reference;
    for (std::size_t step = 0; step < operations.size(); ++step) {
        const auto& operation = operations[step];
        const auto event = as_event(operation, step + 1);
        const auto actual = book.process(event);
        const auto expected = reference.process(event);
        if (logical_output(actual) != logical_output(expected)) {
            return Failure{step, "logical output disagrees with independent reference"};
        }
        if (!same_state(book, reference)) {
            return Failure{step, "book state or FIFO disagrees with independent reference"};
        }
        if (const auto invariant = book.validate_invariants()) {
            return Failure{step, *invariant};
        }
    }
    return std::nullopt;
}

std::vector<Operation> minimize(std::vector<Operation> operations, const std::string& reason) {
    std::size_t chunk = std::max<std::size_t>(1, operations.size() / 2);
    while (chunk >= 1) {
        bool reduced = false;
        for (std::size_t start = 0; start + chunk <= operations.size(); ++start) {
            std::vector<Operation> candidate;
            candidate.reserve(operations.size() - chunk);
            candidate.insert(candidate.end(), operations.begin(), operations.begin() + start);
            candidate.insert(candidate.end(), operations.begin() + start + chunk, operations.end());
            const auto failure = replay(candidate);
            if (!candidate.empty() && failure && failure->message == reason) {
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

Operation generate_operation(std::mt19937_64& random, const ReferenceBook& model_book,
                             lob::OrderId& next_id) {
    std::vector<lob::OrderId> active;
    for (const auto& order : model_book.orders) active.push_back(order.id);
    const int choice = static_cast<int>(random() % 100);
    if (active.empty() || choice < 45) {
        const auto side = random() % 2 == 0 ? lob::Side::Buy : lob::Side::Sell;
        const lob::Price price = 9980 + static_cast<lob::Price>(random() % 41);
        const lob::Quantity quantity = random() % 12 == 0 ? 0 : 1 + random() % 100;
        const auto id = !active.empty() && random() % 8 == 0
            ? active[random() % active.size()] : next_id++;
        Operation operation{OperationType::Submit, id, side, price, quantity};
        return operation;
    }

    const auto stored = model_book.orders[random() % model_book.orders.size()];
    const auto selected_id = stored.id;
    const auto id = random() % 10 == 0 ? next_id + 100 : selected_id;
    if (choice < 65) {
        Operation operation{OperationType::Cancel, id, stored.side, stored.price, stored.quantity};
        return operation;
    }
    if (choice < 85) {
        const lob::Price new_price = random() % 10 == 0 ? 0 : 9980 + static_cast<lob::Price>(random() % 41);
        const lob::Quantity new_quantity = random() % 4 == 0
            ? stored.quantity
            : 1 + random() % 120;
        Operation operation{OperationType::Modify, id, stored.side, new_price, new_quantity};
        return operation;
    }

    const lob::Quantity executed = random() % 5 == 0
        ? stored.quantity + 1 + random() % 10
        : 1 + random() % stored.quantity;
    Operation operation{OperationType::Execute, id, stored.side, stored.price, executed};
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
    ReferenceBook generation_model;
    std::vector<Operation> operations;
    operations.reserve(steps);
    lob::OrderId next_id = 1;
    for (std::size_t i = 0; i < steps; ++i) {
        operations.push_back(generate_operation(random, generation_model, next_id));
        generation_model.process(as_event(operations.back(), i + 1));
    }

    if (const auto failure = replay(operations)) {
        const auto minimized = minimize(operations, failure->message);
        const auto minimized_failure = *replay(minimized);
        save_failure(failure_path, seed, minimized, minimized_failure);
        std::cerr << "property failure: " << failure->message << " at step " << failure->step
                  << "; minimized from " << operations.size() << " to " << minimized.size()
                  << " operations; saved to " << failure_path << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "property_fuzz_passed seed=" << seed << " steps=" << steps << '\n';
}
