#include "text_input.h"

#include "protocol.h"

#include <cstring>
#include <string>

namespace remoe {

std::optional<std::vector<INPUT>> text_input_events(std::span<const std::uint8_t> message) {
    if (message.size() < sizeof(protocol::TextInputHeader)) return std::nullopt;
    protocol::TextInputHeader header;
    std::memcpy(&header, message.data(), sizeof(header));
    if (header.magic != protocol::kTextInputMagic || header.version != protocol::kVersion ||
        header.header_size != sizeof(header) || header.payload_size > protocol::kMaxTextInputSize ||
        message.size() != sizeof(header) + header.payload_size) return std::nullopt;
    if (header.payload_size == 0) return std::vector<INPUT>{};

    const auto* utf8 = reinterpret_cast<const char*>(message.data() + sizeof(header));
    const int size = static_cast<int>(header.payload_size);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, size, text.data(), length) != length ||
        text.find(L'\0') != std::wstring::npos) return std::nullopt;

    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const wchar_t character = text[i];
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.dwExtraInfo = static_cast<ULONG_PTR>(protocol::kInjectedInputMarker);
        if (character == L'\r' || character == L'\n' || character == L'\t') {
            input.ki.wScan = character == L'\t' ? 0x0f : 0x1c;
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
            if (character == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else {
            // UTF-16 surrogate pairs are passed as two ordered Unicode key pairs.
            input.ki.wScan = static_cast<WORD>(character);
            input.ki.dwFlags = KEYEVENTF_UNICODE;
        }
        inputs.push_back(input);
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
        inputs.push_back(input);
    }
    return inputs;
}

} // namespace remoe
