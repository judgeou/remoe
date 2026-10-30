#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace remoe {

// Validates the complete wire message before constructing any input events.
// Returning the events separately lets tests inspect them without typing.
std::optional<std::vector<INPUT>> text_input_events(std::span<const std::uint8_t> message);

} // namespace remoe
