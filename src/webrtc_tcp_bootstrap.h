#pragma once

#include "webrtc_transport.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace remoe {

struct WebRtcTcpBootstrapIo {
    using Deadline = std::chrono::steady_clock::time_point;

    std::function<bool(const void*, std::size_t)> send_all;
    std::function<bool(void*, std::size_t, Deadline)> receive_all;
};

// Establishes DataChannels by exchanging framed SDP and ICE messages over an
// arbitrary reliable byte stream. WebSocket signaling adapts its binary frames
// to this interface; the TCP-shaped name remains for wire-format compatibility.
std::unique_ptr<WebRtcTransport> establish_webrtc_over_tcp(
    WebRtcTransport::Role role,
    WebRtcTcpBootstrapIo io,
    WebRtcTransport::Callbacks callbacks,
    std::chrono::milliseconds timeout = std::chrono::seconds(15),
    std::vector<std::string> ice_servers = {},
    std::optional<WebRtcTransport::VideoCodec> video_codec = std::nullopt);

} // namespace remoe
