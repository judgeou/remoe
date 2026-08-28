#pragma once

#include "webrtc_transport.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace remoe {

struct ManagedHostIdentity {
    std::string device_id;
    std::string token;
};

bool valid_managed_host_identity(const ManagedHostIdentity& identity) noexcept;

// Opens a ten-minute pairing window. A new Host has no identity; --repair passes
// its current identity so the server can transfer the existing Host entry.
ManagedHostIdentity pair_managed_webrtc_host(
    std::string signaling_url,
    std::optional<ManagedHostIdentity> current_identity,
    std::function<void(std::string)> on_pairing_code,
    std::function<bool()> stop_requested = {});

// Creates a shareable invite URL with a cryptographically secure 21-character
// Nano ID in its fragment. The base URL must not already have a fragment.
std::string create_webrtc_signaling_invite(std::string signaling_url);

// Establishes a reliable control DataChannel and standards-based video track,
// relaying only the framed SDP/ICE bootstrap through a WebSocket server. A
// STUN-only URL is derived from the signaling host; TURN is never configured.
std::unique_ptr<WebRtcTransport> establish_webrtc_over_websocket(
    WebRtcTransport::Role role,
    std::string signaling_invite_url,
    WebRtcTransport::Callbacks callbacks,
    std::chrono::milliseconds timeout = std::chrono::seconds(15),
    std::function<bool()> stop_requested = {},
    std::function<void()> on_signaling_open = {},
    WebRtcTransport::VideoCodec video_codec = WebRtcTransport::VideoCodec::AV1);

// Registers a paired Host and waits on the same WebSocket until an authorized
// browser client is assigned by the account service.
std::unique_ptr<WebRtcTransport> establish_managed_host_webrtc(
    std::string signaling_url,
    const ManagedHostIdentity& identity,
    WebRtcTransport::Callbacks callbacks,
    std::chrono::milliseconds timeout = std::chrono::seconds(15),
    std::function<bool()> stop_requested = {},
    std::function<void()> on_signaling_open = {},
    WebRtcTransport::VideoCodec video_codec = WebRtcTransport::VideoCodec::AV1);

} // namespace remoe
