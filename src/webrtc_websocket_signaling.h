#pragma once

#include "webrtc_transport.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace remoe {

// Creates a shareable invite URL with a cryptographically secure 21-character
// Nano ID in its fragment. The base URL must not already have a fragment.
std::string create_webrtc_signaling_invite(std::string signaling_url);

// Establishes reliable control and low-latency video DataChannels, relaying
// only the framed SDP/ICE bootstrap through a WebSocket server. A STUN-only URL
// is derived from the signaling host; TURN is never configured.
std::unique_ptr<WebRtcTransport> establish_webrtc_over_websocket(
    WebRtcTransport::Role role,
    std::string signaling_invite_url,
    WebRtcTransport::Callbacks callbacks,
    std::chrono::milliseconds timeout = std::chrono::seconds(15),
    std::function<bool()> stop_requested = {});

} // namespace remoe
