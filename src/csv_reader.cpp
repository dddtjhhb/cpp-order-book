#include "csv_reader.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace lob {
namespace {

constexpr std::size_t kColumnCount = 6;
constexpr std::string_view kColumnNames[kColumnCount] = {
    "timestamp_ns", "event_type", "order_id", "side", "price_ticks", "quantity"};

Side parse_side(std::string_view text) {
    if (text == "BUY") return Side::Buy;
    if (text == "SELL") return Side::Sell;
    throw std::runtime_error("invalid side: " + std::string(text));
}

EventType parse_type(std::string_view text) {
    if (text == "ADD") return EventType::Add;
    if (text == "CANCEL") return EventType::Cancel;
    if (text == "MODIFY") return EventType::Modify;
    if (text == "EXECUTE") return EventType::Execute;
    throw std::runtime_error("invalid event type: " + std::string(text));
}

// Parses the entire field as a base-10 integer. Unlike std::stoull, this
// rejects a leading '-' for unsigned types (stoull silently wraps "-5" to
// 2^64 - 5), leading whitespace or '+', trailing garbage, and out-of-range values.
template <typename Integer>
Integer parse_integer(std::string_view text, std::string_view column) {
    static_assert(std::is_integral_v<Integer>);
    Integer value{};
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (text.empty() || error == std::errc::invalid_argument || end != last) {
        throw std::runtime_error("invalid integer in " + std::string(column) + ": '" +
                                 std::string(text) + "'");
    }
    if (error == std::errc::result_out_of_range) {
        throw std::runtime_error("out-of-range integer in " + std::string(column) + ": '" +
                                 std::string(text) + "'");
    }
    return value;
}

// Splits a row into exactly kColumnCount fields; any other count is an error.
bool split_fields(std::string_view line, std::string_view (&fields)[kColumnCount]) {
    std::size_t index = 0;
    std::size_t start = 0;
    while (true) {
        const auto comma = line.find(',', start);
        if (index == kColumnCount) return false;  // too many columns
        fields[index++] = line.substr(start, comma == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : comma - start);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return index == kColumnCount;
}

}  // namespace

std::vector<Event> read_events(std::istream& input) {
    std::vector<Event> events;
    std::string raw_line;
    std::size_t line_number = 0;

    while (std::getline(input, raw_line)) {
        ++line_number;
        std::string_view line(raw_line);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);  // CRLF files
        if (line.empty() || (line_number == 1 && line.rfind("timestamp", 0) == 0)) {
            continue;
        }

        std::string_view fields[kColumnCount];
        if (!split_fields(line, fields)) {
            throw std::runtime_error("CSV line " + std::to_string(line_number) +
                                     ": expected exactly " + std::to_string(kColumnCount) +
                                     " columns");
        }

        try {
            events.push_back(Event{
                parse_integer<std::uint64_t>(fields[0], kColumnNames[0]),
                parse_type(fields[1]),
                Order{parse_integer<OrderId>(fields[2], kColumnNames[2]),
                      parse_side(fields[3]),
                      parse_integer<Price>(fields[4], kColumnNames[4]),
                      parse_integer<Quantity>(fields[5], kColumnNames[5])}});
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
