#include "csv_reader.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>

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

template <typename Integer>
Integer parse_integer(const std::string& text) {
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid or out-of-range integer: " + text);
    }
    return value;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        const auto end = line.find(',', start);
        result.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) return result;
        start = end + 1;
    }
}

}  // namespace

std::vector<Event> read_events(std::istream& input) {
    std::vector<Event> events;
    std::string line;
    std::size_t line_number = 0;
    std::size_t schema_columns = 0;
    constexpr std::string_view legacy_header =
        "timestamp_ns,event_type,order_id,side,price_ticks,quantity";
    const std::string extended_header = std::string(legacy_header) + ",symbol_id,sequence";

    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line_number == 1 && (line == legacy_header || line == extended_header)) {
            schema_columns = line == legacy_header ? 6 : 8;
            continue;
        }
        try {
            const auto row = fields(line);
            if ((row.size() != 6 && row.size() != 8) ||
                (schema_columns != 0 && row.size() != schema_columns)) {
                throw std::runtime_error("expected consistent 6-column or 8-column CSV");
            }
            schema_columns = row.size();
            events.push_back(Event{
                parse_integer<std::uint64_t>(row[0]), parse_type(row[1]),
                Order{parse_integer<OrderId>(row[2]), parse_side(row[3]),
                      parse_integer<Price>(row[4]), parse_integer<Quantity>(row[5])},
                row.size() == 8 ? parse_integer<SymbolId>(row[6]) : SymbolId{1},
                row.size() == 8 ? parse_integer<RequestSequence>(row[7])
                                : static_cast<RequestSequence>(events.size() + 1)});
        } catch (const std::exception& error) {
            throw std::runtime_error("CSV line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (input.bad()) throw std::runtime_error("failed while reading CSV");
    return events;
}

std::vector<Event> read_events_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("could not open input file: " + path);
    return read_events(input);
}

}  // namespace lob
