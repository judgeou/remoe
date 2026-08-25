#pragma once

#include "webrtc_transport.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace remoe {

struct WebRtcTcpBootstrapIo {
    using Deadline = std::chrono::steady_clock::time_point;

    std::function<bool(const void*, std::size_t)> send_all;
    std::function<bool(void*, std::size_t, Deadline)> receive_all;
};

// Establishes a DataChannel by exchanging framed SDP and ICE messages over an
// already-connected TCP stream. On success all bootstrap bytes have been
// consumed and the TCP stream can immediately carry video.
std::unique_ptr<WebRtcTransport> establish_webrtc_over_tcp(
    WebRtcTransport::Role role,
    WebRtcTcpBootstrapIo io,
    WebRtcTransport::Callbacks callbacks,
    std::chrono::milliseconds timeout = std::chrono::seconds(15),
    std::vector<std::string> ice_servers = {});

} // namespace remoe
