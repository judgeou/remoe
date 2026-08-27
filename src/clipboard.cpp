#include "clipboard.h"

#include "protocol.h"

#include <Windows.h>

#include <cstring>
#include <stdexcept>

namespace remoe {
namespace {

bool open_clipboard_with_retry() {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(5);
    }
    return false;
}

} // namespace

std::optional<std::string> read_clipboard_text() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard_with_retry()) {
        return std::nullopt;
    }
    struct ClipboardCloser {
        ~ClipboardCloser() { CloseClipboard(); }
    } closer;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) return std::nullopt;
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (!text) return std::nullopt;
    const std::size_t capacity = GlobalSize(data) / sizeof(wchar_t);
    const std::size_t length = wcsnlen_s(text, capacity);
    if (capacity == 0 || length == capacity ||
        length > protocol::kMaxClipboardTextSize) {
        GlobalUnlock(data);
        return std::nullopt;
    }
    std::string utf8;
    if (length != 0) {
        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
            static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (required <= 0 || static_cast<std::size_t>(required) >
                protocol::kMaxClipboardTextSize) {
            GlobalUnlock(data);
            return std::nullopt;
        }
        utf8.resize(static_cast<std::size_t>(required));
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length),
                            utf8.data(), required, nullptr, nullptr);
    }
    GlobalUnlock(data);
    return utf8;
}

bool write_clipboard_text(std::string_view utf8) {
    if (utf8.size() > protocol::kMaxClipboardTextSize) return false;
    int wide_length = 0;
    if (!utf8.empty()) {
        wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
        if (wide_length <= 0) return false;
    }
    const std::size_t bytes = (static_cast<std::size_t>(wide_length) + 1) * sizeof(wchar_t);
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!storage) return false;
    auto* output = static_cast<wchar_t*>(GlobalLock(storage));
    if (!output) {
        GlobalFree(storage);
        return false;
    }
    if (wide_length != 0) {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), output, wide_length);
    }
    output[wide_length] = L'\0';
    GlobalUnlock(storage);
    if (!open_clipboard_with_retry()) {
        GlobalFree(storage);
        return false;
    }
    const bool accepted = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, storage);
    CloseClipboard();
    if (!accepted) GlobalFree(storage);
    return accepted;
}

std::vector<std::uint8_t> make_clipboard_message(std::string_view text,
                                                  std::uint32_t sequence) {
    if (text.size() > protocol::kMaxClipboardTextSize) {
        throw std::length_error("clipboard text exceeds the protocol limit");
    }
    protocol::ClipboardHeader header;
    header.payload_size = static_cast<std::uint32_t>(text.size());
    header.sequence = sequence;
    std::vector<std::uint8_t> message(sizeof(header) + text.size());
    std::memcpy(message.data(), &header, sizeof(header));
    if (!text.empty()) std::memcpy(message.data() + sizeof(header), text.data(), text.size());
    return message;
}

bool validate_clipboard_message(std::span<const std::uint8_t> message) {
    if (message.size() < sizeof(protocol::ClipboardHeader)) return false;
    protocol::ClipboardHeader header;
    std::memcpy(&header, message.data(), sizeof(header));
    return header.magic == protocol::kClipboardMagic &&
        header.version == protocol::kVersion && header.header_size == sizeof(header) &&
        header.payload_size <= protocol::kMaxClipboardTextSize &&
        message.size() == sizeof(header) + header.payload_size;
}

std::string_view clipboard_message_text(std::span<const std::uint8_t> message) {
    return {reinterpret_cast<const char*>(message.data() + sizeof(protocol::ClipboardHeader)),
            message.size() - sizeof(protocol::ClipboardHeader)};
}

} // namespace remoe
