#pragma once

#include "webrtc_transport.h"

#include <chrono>
#include <memory>
#include <string>

namespace remoe {

// Creates a shareable invite URL with a cryptographically random 128-bit
// session ID in its fragment. The base URL must not already have a fragment.
std::string create_webrtc_signaling_invite(std::string signaling_url);

// Establishes the same host-candidate-only control channel as the TCP
// bootstrap, but relays the framed SDP/ICE bytes through a WebSocket server.
// The WebSocket stays alive for the lifetime of the returned transport.
std::unique_ptr<WebRtcTransport> establish_webrtc_over_websocket(
    WebRtcTransport::Role role,
    std::string signaling_invite_url,
    WebRtcTransport::Callbacks callbacks,
    std::chrono::milliseconds timeout = std::chrono::seconds(15));

} // namespace remoe
