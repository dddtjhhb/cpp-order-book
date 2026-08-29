#include "csv_reader.hpp"

#include <fstream>
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
        std::string timestamp, type, id, side, price, quantity;
        if (!std::getline(row, timestamp, ',') || !std::getline(row, type, ',') ||
            !std::getline(row, id, ',') || !std::getline(row, side, ',') ||
            !std::getline(row, price, ',') || !std::getline(row, quantity, ',')) {
            throw std::runtime_error("malformed CSV row at line " + std::to_string(line_number));
        }

        try {
            events.push_back(Event{
                std::stoull(timestamp),
                parse_type(type),
                Order{std::stoull(id), parse_side(side), std::stoll(price), std::stoull(quantity)}});
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
