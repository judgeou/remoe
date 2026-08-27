#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace remoe {

// Text-only Windows clipboard helpers. Text is UTF-8 at this boundary.
std::optional<std::string> read_clipboard_text();
bool write_clipboard_text(std::string_view utf8);

std::vector<std::uint8_t> make_clipboard_message(std::string_view text,
                                                  std::uint32_t sequence);
bool validate_clipboard_message(std::span<const std::uint8_t> message);
std::string_view clipboard_message_text(std::span<const std::uint8_t> message);

} // namespace remoe
