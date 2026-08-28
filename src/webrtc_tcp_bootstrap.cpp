#include "webrtc_tcp_bootstrap.h"

#include "protocol.h"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace remoe {
namespace {

using SignalType = protocol::WebRtcSignalType;

constexpr std::uint32_t kMaxDescriptionSize = 1024 * 1024;
constexpr std::uint32_t kMaxCandidateSize = 16 * 1024;
constexpr std::uint32_t kMaxMetadataSize = 256;

template <typename Callback, typename... Args>
void invoke_callback(const Callback& callback, Args&&... args) noexcept {
    if (!callback) return;
    try {
        callback(std::forward<Args>(args)...);
    } catch (...) {
    }
}

struct BootstrapState {
    WebRtcTcpBootstrapIo io;
    WebRtcTransport::Callbacks application_callbacks;
    std::mutex send_mutex;
    mutable std::mutex error_mutex;
    std::mutex progress_mutex;
    std::condition_variable progress;
    std::string error;
    std::atomic_bool local_open{false};
    std::atomic_bool local_gathering_complete{false};
    std::atomic_bool remote_ready{false};
    std::atomic_bool ready_sent{false};
    std::atomic_bool acknowledgement_sent{false};
    std::atomic_bool remote_acknowledged{false};
    std::atomic_bool handshake_complete{false};

    void fail(std::string message) noexcept {
        {
            std::lock_guard lock(error_mutex);
            if (error.empty()) error = message;
        }
        invoke_callback(application_callbacks.on_error, std::move(message));
        progress.notify_all();
    }

    [[nodiscard]] std::string current_error() const {
        std::lock_guard lock(error_mutex);
        return error;
    }

    bool send_signal(SignalType type, const std::string& value = {},
                     const std::string& metadata = {}) noexcept {
        if (handshake_complete.load()) return true;
        if (value.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            metadata.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            fail("WebRTC bootstrap payload is too large");
            return false;
        }

        protocol::WebRtcSignalHeader header;
        header.type = type;
        header.value_size = static_cast<std::uint32_t>(value.size());
        header.metadata_size = static_cast<std::uint32_t>(metadata.size());

        std::lock_guard lock(send_mutex);
        const bool sent = io.send_all(&header, sizeof(header)) &&
            (value.empty() || io.send_all(value.data(), value.size())) &&
            (metadata.empty() || io.send_all(metadata.data(), metadata.size()));
        if (!sent) fail("Failed to send WebRTC bootstrap message over signaling transport");
        return sent;
    }

    void send_ready() noexcept {
        if (!ready_sent.exchange(true)) send_signal(SignalType::Ready);
    }

    void maybe_send_acknowledgement() noexcept {
        if (!local_open.load() || !local_gathering_complete.load() ||
            !remote_ready.load()) return;
        if (!acknowledgement_sent.exchange(true)) {
            send_signal(SignalType::Acknowledged);
            progress.notify_all();
        }
    }
};

std::string receive_string(const WebRtcTcpBootstrapIo& io, std::uint32_t size,
                           WebRtcTcpBootstrapIo::Deadline deadline) {
    std::string value(size, '\0');
    if (size != 0 && !io.receive_all(value.data(), value.size(), deadline)) {
        throw std::runtime_error("Signaling transport closed during WebRTC bootstrap payload");
    }
    return value;
}

void validate_signal_header(const protocol::WebRtcSignalHeader& header) {
    if (header.magic != protocol::kWebRtcSignalMagic ||
        header.version != protocol::kVersion ||
        header.header_size != sizeof(header) || header.reserved != 0) {
        throw std::runtime_error("Peer sent an invalid WebRTC bootstrap header");
    }

    switch (header.type) {
    case SignalType::Description:
        if (header.value_size == 0 || header.value_size > kMaxDescriptionSize ||
            header.metadata_size == 0 || header.metadata_size > kMaxMetadataSize) {
            throw std::runtime_error("Peer sent an invalid WebRTC description frame");
        }
        break;
    case SignalType::Candidate:
        if (header.value_size == 0 || header.value_size > kMaxCandidateSize ||
            header.metadata_size > kMaxMetadataSize) {
            throw std::runtime_error("Peer sent an invalid WebRTC candidate frame");
        }
        break;
    case SignalType::Ready:
    case SignalType::Acknowledged:
        if (header.value_size != 0 || header.metadata_size != 0) {
            throw std::runtime_error("Peer sent an invalid WebRTC bootstrap marker");
        }
        break;
    default:
        throw std::runtime_error("Peer sent an unknown WebRTC bootstrap message");
    }
}

} // namespace

std::unique_ptr<WebRtcTransport> establish_webrtc_over_tcp(
    WebRtcTransport::Role role, WebRtcTcpBootstrapIo io,
    WebRtcTransport::Callbacks callbacks, std::chrono::milliseconds timeout,
    std::vector<std::string> ice_servers,
    std::optional<WebRtcTransport::VideoCodec> video_codec) {
    if (!io.send_all || !io.receive_all) {
        throw std::invalid_argument("WebRTC TCP bootstrap requires send and receive functions");
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("WebRTC TCP bootstrap timeout must be positive");
    }

    auto state = std::make_shared<BootstrapState>();
    state->io = std::move(io);
    state->application_callbacks = std::move(callbacks);

    WebRtcTransport::Callbacks transport_callbacks;
    transport_callbacks.on_local_description = [state](WebRtcTransport::LocalDescription value) {
        state->send_signal(SignalType::Description, value.sdp, value.type);
        invoke_callback(state->application_callbacks.on_local_description, std::move(value));
    };
    transport_callbacks.on_local_candidate = [state](WebRtcTransport::IceCandidate value) {
        state->send_signal(SignalType::Candidate, value.candidate, value.mid);
        invoke_callback(state->application_callbacks.on_local_candidate, std::move(value));
    };
    transport_callbacks.on_state_changed = [state](WebRtcTransport::State value) {
        invoke_callback(state->application_callbacks.on_state_changed, value);
    };
    transport_callbacks.on_ice_state_changed = [state](WebRtcTransport::IceState value) {
        invoke_callback(state->application_callbacks.on_ice_state_changed, value);
    };
    transport_callbacks.on_gathering_state_changed =
        [state](WebRtcTransport::GatheringState value) {
            if (value == WebRtcTransport::GatheringState::Complete) {
                state->local_gathering_complete = true;
                state->maybe_send_acknowledgement();
                state->progress.notify_all();
            }
            invoke_callback(state->application_callbacks.on_gathering_state_changed, value);
        };
    transport_callbacks.on_open = [state] {
        state->local_open = true;
        state->send_ready();
        state->maybe_send_acknowledgement();
        state->progress.notify_all();
        invoke_callback(state->application_callbacks.on_open);
    };
    transport_callbacks.on_video_open = [state] {
        invoke_callback(state->application_callbacks.on_video_open);
    };
    transport_callbacks.on_closed = [state] {
        invoke_callback(state->application_callbacks.on_closed);
    };
    transport_callbacks.on_text = [state](std::string value) {
        invoke_callback(state->application_callbacks.on_text, std::move(value));
    };
    transport_callbacks.on_binary = [state](std::vector<std::uint8_t> value) {
        invoke_callback(state->application_callbacks.on_binary, std::move(value));
    };
    transport_callbacks.on_video_frame = [state](std::vector<std::uint8_t> value,
                                                  std::uint64_t timestamp_us,
                                                  bool key_frame) {
        invoke_callback(state->application_callbacks.on_video_frame,
                        std::move(value), timestamp_us, key_frame);
    };
    transport_callbacks.on_video_keyframe_requested = [state] {
        invoke_callback(state->application_callbacks.on_video_keyframe_requested);
    };
    transport_callbacks.on_diagnostic = [state](std::string value) {
        invoke_callback(state->application_callbacks.on_diagnostic, std::move(value));
    };
    transport_callbacks.on_error = [state](std::string value) {
        state->fail(std::move(value));
    };

    WebRtcTransport::Configuration configuration;
    configuration.role = role;
    configuration.ice_servers = std::move(ice_servers);
    if (video_codec) {
        configuration.video_codec = *video_codec;
        configuration.video_direction = role == WebRtcTransport::Role::Offerer
            ? WebRtcTransport::VideoDirection::ReceiveOnly
            : WebRtcTransport::VideoDirection::SendOnly;
    }
    auto transport = std::make_unique<WebRtcTransport>(
        std::move(configuration), std::move(transport_callbacks));

    const auto deadline = timeout == (std::chrono::milliseconds::max)()
        ? std::chrono::steady_clock::time_point::max()
        : std::chrono::steady_clock::now() + timeout;
    bool remote_description_received = false;
    std::vector<WebRtcTransport::IceCandidate> pending_candidates;

    try {
        transport->start();
        while (!(state->local_open.load() && state->remote_acknowledged.load())) {
            if (const std::string error = state->current_error(); !error.empty()) {
                throw std::runtime_error(error);
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("Timed out establishing the WebRTC control channel");
            }

            protocol::WebRtcSignalHeader header;
            if (!state->io.receive_all(&header, sizeof(header), deadline)) {
                throw std::runtime_error("Signaling transport closed during WebRTC bootstrap");
            }
            validate_signal_header(header);
            std::string value = receive_string(state->io, header.value_size, deadline);
            std::string metadata = receive_string(state->io, header.metadata_size, deadline);

            switch (header.type) {
            case SignalType::Description:
                if (remote_description_received) {
                    throw std::runtime_error("Peer sent multiple WebRTC descriptions");
                }
                transport->set_remote_description(value, metadata);
                remote_description_received = true;
                for (const auto& candidate : pending_candidates) {
                    transport->add_remote_candidate(candidate.candidate, candidate.mid);
                }
                pending_candidates.clear();
                break;
            case SignalType::Candidate:
                if (remote_description_received) {
                    transport->add_remote_candidate(value, metadata);
                } else {
                    pending_candidates.push_back({std::move(value), std::move(metadata)});
                }
                break;
            case SignalType::Ready:
                state->remote_ready = true;
                state->maybe_send_acknowledgement();
                break;
            case SignalType::Acknowledged:
                state->remote_acknowledged = true;
                break;
            }
        }
        {
            std::unique_lock lock(state->progress_mutex);
            state->progress.wait_until(lock, deadline, [&] {
                return state->acknowledgement_sent.load() || !state->current_error().empty();
            });
        }
        if (!state->acknowledgement_sent.load()) {
            if (const std::string error = state->current_error(); !error.empty()) {
                throw std::runtime_error(error);
            }
            throw std::runtime_error("Timed out completing local ICE candidate gathering");
        }
        state->handshake_complete = true;
        return transport;
    } catch (...) {
        transport->close();
        throw;
    }
}

} // namespace remoe
