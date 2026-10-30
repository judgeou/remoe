#include "protocol.h"
#include "text_input.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void expect(bool value, std::string_view description) {
    if (!value) throw std::runtime_error(std::string(description));
}

std::vector<std::uint8_t> message(std::string_view text) {
    remoe::protocol::TextInputHeader header;
    header.payload_size = static_cast<std::uint32_t>(text.size());
    std::vector<std::uint8_t> bytes(sizeof(header) + text.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), text.data(), text.size());
    return bytes;
}
} // namespace

int main() {
    // Shared with Android's TextInputCodecTest, including unsigned sequence.
    const std::vector<std::uint8_t> golden{
        0x54, 0x45, 0x58, 0x54, 0x0b, 0x00, 0x10, 0x00,
        0x07, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
        0xe4, 0xb8, 0xad, 0xf0, 0x9f, 0x9a, 0x80,
    };
    const auto unicode = remoe::text_input_events(golden);
    expect(unicode && unicode->size() == 6, "Chinese and surrogate pair produce six events");
    const WORD units[]{0x4e2d, 0xd83d, 0xde80};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& down = (*unicode)[i * 2];
        const auto& up = (*unicode)[i * 2 + 1];
        expect(down.type == INPUT_KEYBOARD && down.ki.wVk == 0 && down.ki.wScan == units[i] &&
               down.ki.dwFlags == KEYEVENTF_UNICODE &&
               down.ki.dwExtraInfo == remoe::protocol::kInjectedInputMarker, "Unicode down event");
        expect(up.ki.wScan == units[i] && up.ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
               "Unicode release event");
    }
    const auto lines = remoe::text_input_events(message("a\r\nb\nc\rd\t"));
    expect(lines && lines->size() == 16, "CRLF produces only one Enter");
    for (std::size_t index : {2u, 6u, 10u, 14u}) {
        expect((*lines)[index].ki.dwFlags == KEYEVENTF_SCANCODE, "line ending and tab use scan codes");
        expect((*lines)[index].ki.wScan == (index == 14 ? 0x0f : 0x1c), "Enter/Tab scan code");
    }
    expect(remoe::text_input_events(message(""))->empty(), "empty text is a no-op");
    expect(remoe::text_input_events(message(std::string(4096, 'x')))->size() == 8192, "maximum payload");
    expect(!remoe::text_input_events(message(std::string(4097, 'x'))), "oversized payload rejected");
    for (const auto invalid : {std::string_view("\xc3\x28", 2), std::string_view("\xc0\x80", 2),
                               std::string_view("\xed\xa0\x80", 3), std::string_view("a\0b", 3)}) {
        expect(!remoe::text_input_events(message(invalid)), "invalid Unicode rejected before injection");
    }
    for (std::size_t offset : {0u, 4u, 6u, 8u}) {
        auto invalid = golden;
        invalid[offset] ^= 1;
        expect(!remoe::text_input_events(invalid), "invalid header rejected");
    }
    auto truncated = golden;
    truncated.pop_back();
    expect(!remoe::text_input_events(truncated), "truncated payload rejected");
    expect(!remoe::text_input_events(std::span(golden).first(15)), "truncated header rejected");
    std::cout << "Text input protocol and Unicode event tests passed\n";
}
