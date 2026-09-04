#pragma once

#include <cstddef>
#include <cstdint>

namespace remoe::protocol {

constexpr std::uint32_t kStreamMagic = 0x454F4D52; // "RMOE" on the wire (little-endian)
constexpr std::uint32_t kClientConfigMagic = 0x46434D52; // "RMCF"
constexpr std::uint32_t kInputMagic = 0x54504E49; // "INPT"
constexpr std::uint32_t kClipboardMagic = 0x50494C43; // "CLIP"
constexpr std::uint32_t kWebRtcSignalMagic = 0x534D5257; // "WRMS"
constexpr std::uint32_t kStreamReadyMagic = 0x59445253; // "SRDY"
constexpr std::uint32_t kStreamStatusMagic = 0x54534D52; // "RMST"
constexpr std::uint32_t kVideoChunkMagic = 0x4B484356; // "VCHK"
constexpr std::uint32_t kClockSyncMagic = 0x4B4C4343; // "CCLK"
constexpr std::uint32_t kCursorStateMagic = 0x53525543; // "CURS"
constexpr std::uint16_t kVersion = 11;
constexpr std::uint32_t kCodecAv1 = 0x31305641;   // "AV01"
constexpr std::uint32_t kCodecH264 = 0x34363248;  // "H264"
constexpr std::size_t kMaxClipboardTextSize = 1024 * 1024;
constexpr std::size_t kVideoChunkPayloadSize = 16 * 1024;
// Identifies Windows input synthesized by a remoe host. A client connected to
// a host on the same machine must not send that input back to the host.
constexpr std::uintptr_t kInjectedInputMarker =
    static_cast<std::uintptr_t>(UINT64_C(0x524D4F45494E5054)); // "RMOEINPT"

enum ClientFlags : std::uint32_t {
    kClientClipboardText = 1u << 0,
    kClientStreamStatus = 1u << 1,
    kClientLowLatencyVideo = 1u << 2,
    kClientCursorState = 1u << 3,
};

enum CursorFlags : std::uint32_t {
    kCursorVisible = 1u << 0,
    kCursorEmbeddedInVideo = 1u << 1,
    kCursorInsideOutput = 1u << 2,
};

enum FrameFlags : std::uint32_t {
    kFrameKey = 1u << 0,
};

enum class VideoRateControl : std::uint32_t {
    Cbr = 0,
    FixedQuality = 1,
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
    MouseMoveRelative = 11,
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
    // Nominal network media rate used to configure RTP pacing. In CBR mode it
    // is also the encoder target; fixed-quality mode uses quality for encoding.
    std::uint32_t bitrate_bps = 20'000'000;
    std::uint32_t scale_percent = 100;
    std::uint32_t flags = 0;
    VideoRateControl rate_control = VideoRateControl::Cbr;
    // FixedQuality uses a 1..51 scale where a smaller value means higher quality.
    std::uint32_t quality = 0;
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
    // Echoes ClientConfig::bitrate_bps, including in fixed-quality mode.
    std::uint32_t bitrate_bps = 0;
    // H.264 uses the low 24 bits for profile_idc, constraint flags and level_idc.
    // AV1 leaves this field at zero.
    std::uint32_t codec_profile = 0;
    VideoRateControl rate_control = VideoRateControl::Cbr;
    std::uint32_t quality = 0;
};

struct StreamReady {
    std::uint32_t magic = kStreamReadyMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(StreamReady);
};

// Optional host-to-client telemetry, advertised with kClientStreamStatus.
// media_bitrate_bps is the encoder's current target (zero for fixed-quality).
struct StreamStatus {
    std::uint32_t magic = kStreamStatusMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(StreamStatus);
    std::uint32_t media_bitrate_bps = 0;
    std::uint64_t pacing_bitrate_bps = 0;
};

// Optional host-to-client system cursor state, advertised with
// kClientCursorState. Coordinates are normalized to the captured output.
struct CursorState {
    std::uint32_t magic = kCursorStateMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(CursorState);
    std::uint32_t flags = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t sequence = 0;
};

// One unordered, non-retransmitted low-latency video-channel message. Frames
// are split into fixed-size chunks below common SCTP message-size limits.
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

struct ClockSyncRequest {
    std::uint32_t magic = kClockSyncMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(ClockSyncRequest);
    std::uint32_t sequence = 0;
    std::uint32_t reserved = 0;
    std::uint64_t client_send_us = 0;
};

struct ClockSyncResponse {
    std::uint32_t magic = kClockSyncMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(ClockSyncResponse);
    std::uint32_t sequence = 0;
    std::uint32_t reserved = 0;
    std::uint64_t client_send_us = 0;
    std::uint64_t host_receive_us = 0;
    std::uint64_t host_send_us = 0;
};

// Client-to-host input message. MouseMove values are normalized to 0..65535;
// MouseMoveRelative values are signed mouse deltas. Keyboard value1 is a
// Windows scan code; value2 is unused.
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

// Bidirectional UTF-8 clipboard text. The payload immediately follows this
// header and is not NUL-terminated. Clipboard images/files are deliberately
// excluded so a peer cannot accidentally transfer an unbounded object.
struct ClipboardHeader {
    std::uint32_t magic = kClipboardMagic;
    std::uint16_t version = kVersion;
    std::uint16_t header_size = sizeof(ClipboardHeader);
    std::uint32_t payload_size = 0;
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

static_assert(sizeof(ClientConfig) == 36);
static_assert(sizeof(StreamHeader) == 44);
static_assert(sizeof(StreamReady) == 8);
static_assert(sizeof(StreamStatus) == 20);
static_assert(sizeof(CursorState) == 24);
static_assert(sizeof(VideoChunkHeader) == 36);
static_assert(sizeof(ClockSyncRequest) == 24);
static_assert(sizeof(ClockSyncResponse) == 40);
static_assert(sizeof(InputEvent) == 24);
static_assert(sizeof(ClipboardHeader) == 16);
static_assert(sizeof(WebRtcSignalHeader) == 20);

} // namespace remoe::protocol
