#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef REMOE_ENABLE_NATIVE_VIDEO_RECEIVER
#define REMOE_ENABLE_NATIVE_VIDEO_RECEIVER 0
#endif

namespace remoe {

// A signaling-agnostic WebRTC transport with one reliable control DataChannel
// and an optional standards-based receive/send video track. The caller is
// responsible for carrying LocalDescription and IceCandidate values to the
// remote peer.
// Callbacks run on libdatachannel worker threads and must return promptly.
class WebRtcTransport final {
public:
    enum class Role {
        Offerer,
        Answerer,
    };

    enum class VideoCodec {
        H264,
        AV1,
    };

    enum class VideoDirection {
        Disabled,
        SendOnly,
        ReceiveOnly,
    };

    enum class State {
        New,
        Connecting,
        Connected,
        Disconnected,
        Failed,
        Closed,
    };

    enum class IceState {
        New,
        Checking,
        Connected,
        Completed,
        Failed,
        Disconnected,
        Closed,
    };

    enum class GatheringState {
        New,
        InProgress,
        Complete,
    };

    struct Configuration {
        Role role = Role::Answerer;
        std::string data_channel_label = "remoe-control";
        VideoDirection video_direction = VideoDirection::Disabled;
        VideoCodec video_codec = VideoCodec::H264;
        // Only explicit stun: URLs are accepted; TURN is intentionally disabled.
        std::vector<std::string> ice_servers;
        std::optional<std::string> bind_address;
        std::uint16_t port_range_begin = 1024;
        std::uint16_t port_range_end = 65535;
        bool enable_ice_tcp = false;
    };

    struct LocalDescription {
        std::string sdp;
        std::string type;
    };

    struct IceCandidate {
        std::string candidate;
        std::string mid;
    };

    struct Statistics {
        std::uint64_t bytes_sent = 0;
        std::uint64_t bytes_received = 0;
        std::optional<std::chrono::milliseconds> round_trip_time;
        std::optional<IceCandidate> local_candidate;
        std::optional<IceCandidate> remote_candidate;
    };

    struct VideoFeedback {
        bool receiver_report = false;
        // RTCP fraction lost converted from the RFC 3550 8-bit fixed point
        // field into the range [0, 1].
        double loss_fraction = 0.0;
        std::uint32_t cumulative_packets_lost = 0;
        std::uint32_t jitter = 0;
        std::uint32_t nack_packets = 0;
    };

    struct VideoPacingStatistics {
        std::uint64_t pacing_bitrate_bps = 0;
        std::chrono::milliseconds pacing_interval{0};
        std::size_t queued_bytes = 0;
        std::size_t queued_packets = 0;
        double queue_delay_ms = 0.0;
        double scheduler_lateness_ms = 0.0;
        std::uint64_t dropped_batches = 0;
        std::uint64_t dropped_packets = 0;
    };

    struct Callbacks {
        std::function<void(LocalDescription)> on_local_description;
        std::function<void(IceCandidate)> on_local_candidate;
        std::function<void(State)> on_state_changed;
        std::function<void(IceState)> on_ice_state_changed;
        std::function<void(GatheringState)> on_gathering_state_changed;
        std::function<void()> on_open;
        std::function<void()> on_video_open;
        std::function<void()> on_closed;
        std::function<void(std::string)> on_text;
        std::function<void(std::vector<std::uint8_t>)> on_binary;
#if REMOE_ENABLE_NATIVE_VIDEO_RECEIVER
        // Native-client compatibility: receives one complete encoded video
        // access unit after RTP depacketization. timestamp_us is derived from
        // the 90 kHz RTP clock.
        std::function<void(std::vector<std::uint8_t>, std::uint64_t timestamp_us,
                           bool key_frame)> on_video_frame;
#endif
        // Raised for RTCP PLI/FIR feedback received by a sending track.
        std::function<void()> on_video_keyframe_requested;
        // Receiver reports and NACK counts used by the host-side adaptive
        // bitrate controller.
        std::function<void(VideoFeedback)> on_video_feedback;
        // Raised when the bounded video pacer has to reject a complete encoded
        // frame. The sender should force the next accepted frame to be a key frame.
        std::function<void()> on_video_pacing_overflow;
#if REMOE_ENABLE_NATIVE_VIDEO_RECEIVER
        // Low-volume receive diagnostics intended for persistent native-client logs.
        std::function<void(std::string)> on_diagnostic;
#endif
        std::function<void(std::string)> on_error;
    };

    WebRtcTransport(Configuration configuration, Callbacks callbacks);
    ~WebRtcTransport();

    WebRtcTransport(const WebRtcTransport&) = delete;
    WebRtcTransport& operator=(const WebRtcTransport&) = delete;
    WebRtcTransport(WebRtcTransport&&) = delete;
    WebRtcTransport& operator=(WebRtcTransport&&) = delete;

    // Starts negotiation. For an offerer this creates the configured data
    // channel and emits an offer. An answerer waits for a remote offer.
    void start();

    void set_remote_description(std::string_view sdp, std::string_view type);
    void add_remote_candidate(std::string_view candidate, std::string_view mid);

    // Returns true when the message was accepted. libdatachannel may queue it
    // internally for SCTP backpressure; that is still considered success.
    [[nodiscard]] bool send_text(std::string_view message) noexcept;
    [[nodiscard]] bool send_binary(std::span<const std::uint8_t> message) noexcept;
    // Sends one complete encoded access unit through the configured RTP
    // packetizer. The timestamp is expressed in the caller's monotonic epoch.
    [[nodiscard]] bool send_video_frame(std::span<const std::uint8_t> frame,
                                        std::uint64_t timestamp_us) noexcept;
    // Installs the sender-side RTP pacer before the first video frame. The
    // supplied rate is the actual wire pacing rate, not the encoder target.
    [[nodiscard]] bool configure_video_pacing(std::uint64_t bitrate_bps) noexcept;
    [[nodiscard]] bool update_video_pacing(
        std::uint64_t bitrate_bps, std::chrono::milliseconds interval) noexcept;
    [[nodiscard]] VideoPacingStatistics video_pacing_statistics() const noexcept;
#if REMOE_ENABLE_NATIVE_VIDEO_RECEIVER
    [[nodiscard]] bool request_video_keyframe() noexcept;
#endif

    void close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] IceState ice_state() const noexcept;
    [[nodiscard]] GatheringState gathering_state() const noexcept;
    [[nodiscard]] std::size_t buffered_amount() const noexcept;
    [[nodiscard]] bool is_video_open() const noexcept;
    [[nodiscard]] Statistics statistics() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace remoe
