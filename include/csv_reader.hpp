#pragma once

#include "event.hpp"

#include <istream>
#include <string>
#include <vector>

namespace lob {

std::vector<Event> read_events(std::istream& input);
std::vector<Event> read_events_file(const std::string& path);

}  // namespace lob
