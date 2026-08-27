#pragma once

#include <cstddef>
#include <cstdint>

namespace remoe::protocol {

constexpr std::uint32_t kStreamMagic = 0x454F4D52; // "RMOE" on the wire (little-endian)
constexpr std::uint32_t kFrameMagic = 0x4D415246;  // "FRAM"
constexpr std::uint32_t kClientConfigMagic = 0x46434D52; // "RMCF"
constexpr std::uint32_t kInputMagic = 0x54504E49; // "INPT"
constexpr std::uint32_t kWebRtcSignalMagic = 0x534D5257; // "WRMS"
constexpr std::uint32_t kStreamReadyMagic = 0x59445253; // "SRDY"
constexpr std::uint32_t kVideoChunkMagic = 0x4B484356; // "VCHK"
constexpr std::uint16_t kVersion = 7;
constexpr std::uint32_t kCodecAv1 = 0x31305641;   // "AV01"
constexpr std::uint32_t kCodecH264 = 0x34363248;  // "H264"
constexpr std::size_t kVideoChunkPayloadSize = 16 * 1024;

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

enum class WebRtcSignalType : std::uint16_t {
    Description = 1,
    Candidate = 2,
    Ready = 3,
    Acknowledged = 4,
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
    std::uint32_t flags = 0;
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
    // H.264 uses the low 24 bits for profile_idc, constraint flags and level_idc.
    // AV1 leaves this field at zero.
    std::uint32_t codec_profile = 0;
};

struct StreamReady {
    std::uint32_t magic = kStreamReadyMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(StreamReady);
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

// One unordered, non-retransmitted video DataChannel message. A frame is split
// into fixed-size chunks so it stays below the negotiated SCTP message limit.
struct VideoChunkHeader {
    std::uint32_t magic = kVideoChunkMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(VideoChunkHeader);
    std::uint32_t flags = 0;
    std::uint64_t frame_number = 0;
    std::uint64_t timestamp_us = 0;
    std::uint32_t frame_size = 0;
    std::uint32_t chunk_offset = 0;
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

// Framing used only during the TCP bootstrap that precedes the video stream.
// value/metadata contain SDP+type or candidate+mid respectively.
struct WebRtcSignalHeader {
    std::uint32_t magic = kWebRtcSignalMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(WebRtcSignalHeader);
    WebRtcSignalType type = WebRtcSignalType::Description;
    std::uint16_t reserved = 0;
    std::uint32_t value_size = 0;
    std::uint32_t metadata_size = 0;
};
#pragma pack(pop)

static_assert(sizeof(ClientConfig) == 28);
static_assert(sizeof(StreamHeader) == 36);
static_assert(sizeof(StreamReady) == 8);
static_assert(sizeof(FrameHeader) == 32);
static_assert(sizeof(VideoChunkHeader) == 36);
static_assert(sizeof(InputEvent) == 24);
static_assert(sizeof(WebRtcSignalHeader) == 20);

} // namespace remoe::protocol
