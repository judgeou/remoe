#pragma once

#include <cstdint>

namespace remoe::protocol {

constexpr std::uint32_t kStreamMagic = 0x454F4D52; // "RMOE" on the wire (little-endian)
constexpr std::uint32_t kFrameMagic = 0x4D415246;  // "FRAM"
constexpr std::uint32_t kClientConfigMagic = 0x46434D52; // "RMCF"
constexpr std::uint32_t kInputMagic = 0x54504E49; // "INPT"
constexpr std::uint16_t kVersion = 5;
constexpr std::uint32_t kCodecAv1 = 0x31305641;   // "AV01"

enum FrameFlags : std::uint32_t {
    kFrameKey = 1u << 0,
    kFrameConfig = 1u << 1,
};

enum class InputType : std::uint16_t {
    MouseMove = 1,
    MouseLeft = 2,
    MouseRight = 3,
    MouseMiddle = 4,
    MouseX1 = 5,
    MouseX2 = 6,
    MouseWheel = 7,
    MouseHorizontalWheel = 8,
    Keyboard = 9,
    RequestKeyFrame = 10,
};

enum InputFlags : std::uint16_t {
    kInputRelease = 1u << 0,
    kInputExtendedKey = 1u << 1,
};

#pragma pack(push, 1)
struct ClientConfig {
    std::uint32_t magic = kClientConfigMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(ClientConfig);
    std::uint32_t fps_num = 60;
    std::uint32_t fps_den = 1;
    std::uint32_t bitrate_bps = 20'000'000;
    std::uint32_t scale_percent = 100;
    std::uint32_t reserved = 0;
};

struct StreamHeader {
    std::uint32_t magic = kStreamMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(StreamHeader);
    std::uint32_t codec = kCodecAv1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_num = 0;
    std::uint32_t fps_den = 1;
    std::uint32_t bitrate_bps = 0;
    std::uint32_t reserved = 0;
};

struct FrameHeader {
    std::uint32_t magic = kFrameMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(FrameHeader);
    std::uint32_t payload_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t frame_number = 0;
    std::uint64_t timestamp_us = 0;
};

// Client-to-host input message. MouseMove values are normalized to 0..65535.
// Keyboard value1 is a Windows scan code; value2 is unused.
struct InputEvent {
    std::uint32_t magic = kInputMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(InputEvent);
    InputType type = InputType::MouseMove;
    std::uint16_t flags = 0;
    std::int32_t value1 = 0;
    std::int32_t value2 = 0;
    std::uint32_t sequence = 0;
};
#pragma pack(pop)

static_assert(sizeof(ClientConfig) == 28);
static_assert(sizeof(StreamHeader) == 36);
static_assert(sizeof(FrameHeader) == 32);
static_assert(sizeof(InputEvent) == 24);

} // namespace remoe::protocol
