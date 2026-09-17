#include "csv_reader.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace lob {
namespace {

Side parse_side(const std::string& text) {
    if (text == "BUY") return Side::Buy;
    if (text == "SELL") return Side::Sell;
    throw std::runtime_error("invalid side: " + text);
}

EventType parse_type(const std::string& text) {
    if (text == "ADD") return EventType::Add;
    if (text == "CANCEL") return EventType::Cancel;
    if (text == "MODIFY") return EventType::Modify;
    if (text == "EXECUTE") return EventType::Execute;
    throw std::runtime_error("invalid event type: " + text);
}

SymbolId parse_symbol_id(const std::string& text) {
    const auto value = std::stoull(text);
    if (value > std::numeric_limits<SymbolId>::max()) {
        throw std::out_of_range("symbol_id exceeds uint32 range");
    }
    return static_cast<SymbolId>(value);
}

}  // namespace

std::vector<Event> read_events(std::istream& input) {
    std::vector<Event> events;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || (line_number == 1 && line.rfind("timestamp", 0) == 0)) {
            continue;
        }

        std::istringstream row(line);
        std::string timestamp, type, id, side, price, quantity, symbol, sequence;
        if (!std::getline(row, timestamp, ',') || !std::getline(row, type, ',') ||
            !std::getline(row, id, ',') || !std::getline(row, side, ',') ||
            !std::getline(row, price, ',') || !std::getline(row, quantity, ',')) {
            throw std::runtime_error("malformed CSV row at line " + std::to_string(line_number));
        }

        try {
            Event event{
                std::stoull(timestamp),
                parse_type(type),
                Order{std::stoull(id), parse_side(side), std::stoll(price), std::stoull(quantity)}};
            if (std::getline(row, symbol, ',')) {
                if (symbol.empty()) throw std::runtime_error("empty symbol_id");
                event.symbol_id = parse_symbol_id(symbol);
                if (!std::getline(row, sequence, ',') || sequence.empty()) {
                    throw std::runtime_error("symbol_id requires sequence");
                }
                event.sequence = std::stoull(sequence);
            }
            events.push_back(event);
        } catch (const std::exception& error) {
            throw std::runtime_error("CSV line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    return events;
}

std::vector<Event> read_events_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open input file: " + path);
    }
    return read_events(input);
}

}  // namespace lob
