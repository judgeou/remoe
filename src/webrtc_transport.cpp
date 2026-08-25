#include "webrtc_transport.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace remoe {
namespace {

WebRtcTransport::State to_public_state(rtc::PeerConnection::State state) noexcept {
    switch (state) {
    case rtc::PeerConnection::State::New: return WebRtcTransport::State::New;
    case rtc::PeerConnection::State::Connecting: return WebRtcTransport::State::Connecting;
    case rtc::PeerConnection::State::Connected: return WebRtcTransport::State::Connected;
    case rtc::PeerConnection::State::Disconnected: return WebRtcTransport::State::Disconnected;
    case rtc::PeerConnection::State::Failed: return WebRtcTransport::State::Failed;
    case rtc::PeerConnection::State::Closed: return WebRtcTransport::State::Closed;
    }
    return WebRtcTransport::State::Failed;
}

WebRtcTransport::IceState to_public_ice_state(rtc::PeerConnection::IceState state) noexcept {
    switch (state) {
    case rtc::PeerConnection::IceState::New: return WebRtcTransport::IceState::New;
    case rtc::PeerConnection::IceState::Checking: return WebRtcTransport::IceState::Checking;
    case rtc::PeerConnection::IceState::Connected: return WebRtcTransport::IceState::Connected;
    case rtc::PeerConnection::IceState::Completed: return WebRtcTransport::IceState::Completed;
    case rtc::PeerConnection::IceState::Failed: return WebRtcTransport::IceState::Failed;
    case rtc::PeerConnection::IceState::Disconnected: return WebRtcTransport::IceState::Disconnected;
    case rtc::PeerConnection::IceState::Closed: return WebRtcTransport::IceState::Closed;
    }
    return WebRtcTransport::IceState::Failed;
}

WebRtcTransport::GatheringState to_public_gathering_state(
    rtc::PeerConnection::GatheringState state) noexcept {
    switch (state) {
    case rtc::PeerConnection::GatheringState::New: return WebRtcTransport::GatheringState::New;
    case rtc::PeerConnection::GatheringState::InProgress:
        return WebRtcTransport::GatheringState::InProgress;
    case rtc::PeerConnection::GatheringState::Complete:
        return WebRtcTransport::GatheringState::Complete;
    }
    return WebRtcTransport::GatheringState::New;
}

WebRtcTransport::IceCandidate to_public_candidate(const rtc::Candidate& candidate) {
    return {std::string(candidate), candidate.mid()};
}

template <typename Callback, typename... Args>
void invoke_callback(const Callback& callback, Args&&... args) noexcept {
    if (!callback) return;
    try {
        callback(std::forward<Args>(args)...);
    } catch (...) {
        // Never allow application exceptions to cross a libdatachannel thread.
    }
}

} // namespace

struct WebRtcTransport::Impl : std::enable_shared_from_this<WebRtcTransport::Impl> {
    Configuration configuration;
    Callbacks callbacks;
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    std::shared_ptr<rtc::DataChannel> data_channel;
    mutable std::mutex channel_mutex;
    std::atomic<State> connection_state{State::New};
    std::atomic<IceState> current_ice_state{IceState::New};
    std::atomic<GatheringState> current_gathering_state{GatheringState::New};
    std::atomic_bool started{false};
    std::atomic_bool open{false};
    std::atomic_bool closing{false};
    std::atomic_bool closed_notified{false};

    Impl(Configuration config, Callbacks callback_set)
        : configuration(std::move(config)), callbacks(std::move(callback_set)) {}

    void initialize() {
        if (configuration.data_channel_label.empty()) {
            throw std::invalid_argument("WebRTC data channel label must not be empty");
        }
        if (configuration.port_range_begin > configuration.port_range_end) {
            throw std::invalid_argument("WebRTC ICE port range is invalid");
        }

        static std::once_flag logger_once;
        std::call_once(logger_once, [] { rtc::InitLogger(rtc::LogLevel::Warning); });

        rtc::Configuration rtc_config;
        for (const auto& url : configuration.ice_servers) {
            rtc::IceServer server(url);
            if (server.type != rtc::IceServer::Type::Stun || !url.starts_with("stun:")) {
                throw std::invalid_argument("WebRTC transport accepts STUN servers only");
            }
            rtc_config.iceServers.push_back(std::move(server));
        }
        rtc_config.bindAddress = configuration.bind_address;
        rtc_config.portRangeBegin = configuration.port_range_begin;
        rtc_config.portRangeEnd = configuration.port_range_end;
        rtc_config.enableIceTcp = configuration.enable_ice_tcp;

        peer_connection = std::make_shared<rtc::PeerConnection>(std::move(rtc_config));
        const std::weak_ptr<Impl> weak_self = weak_from_this();

        peer_connection->onLocalDescription([weak_self](rtc::Description description) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                invoke_callback(self->callbacks.on_local_description,
                    LocalDescription{std::string(description), description.typeString()});
            }
        });
        peer_connection->onLocalCandidate([weak_self](rtc::Candidate candidate) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                invoke_callback(self->callbacks.on_local_candidate, to_public_candidate(candidate));
            }
        });
        peer_connection->onStateChange([weak_self](rtc::PeerConnection::State state) {
            if (const auto self = weak_self.lock()) {
                const State public_state = to_public_state(state);
                self->connection_state.store(public_state);
                invoke_callback(self->callbacks.on_state_changed, public_state);
            }
        });
        peer_connection->onIceStateChange([weak_self](rtc::PeerConnection::IceState state) {
            if (const auto self = weak_self.lock()) {
                const IceState public_state = to_public_ice_state(state);
                self->current_ice_state.store(public_state);
                invoke_callback(self->callbacks.on_ice_state_changed, public_state);
            }
        });
        peer_connection->onGatheringStateChange(
            [weak_self](rtc::PeerConnection::GatheringState state) {
                if (const auto self = weak_self.lock()) {
                    const GatheringState public_state = to_public_gathering_state(state);
                    self->current_gathering_state.store(public_state);
                    invoke_callback(self->callbacks.on_gathering_state_changed, public_state);
                }
            });
        peer_connection->onDataChannel([weak_self](std::shared_ptr<rtc::DataChannel> channel) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                if (self->configuration.role != Role::Answerer) {
                    channel->close();
                    self->report_error("Unexpected remote-created WebRTC data channel");
                    return;
                }
                if (channel->label() != self->configuration.data_channel_label) {
                    const std::string label = channel->label();
                    channel->close();
                    self->report_error("Unexpected WebRTC data channel label: " + label);
                    return;
                }
                self->attach_data_channel(std::move(channel));
            }
        });
    }

    void start() {
        if (closing.load()) throw std::logic_error("WebRTC transport is closed");
        if (started.exchange(true)) throw std::logic_error("WebRTC transport is already started");
        if (configuration.role == Role::Offerer) {
            attach_data_channel(peer_connection->createDataChannel(configuration.data_channel_label));
        }
    }

    void attach_data_channel(std::shared_ptr<rtc::DataChannel> channel) {
        bool already_attached = false;
        {
            std::lock_guard lock(channel_mutex);
            if (data_channel && !data_channel->isClosed()) {
                already_attached = true;
            } else {
                data_channel = channel;
            }
        }
        if (already_attached) {
            channel->close();
            report_error("WebRTC transport already has a data channel");
            return;
        }

        const std::weak_ptr<Impl> weak_self = weak_from_this();
        channel->onOpen([weak_self] {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                self->open.store(true);
                invoke_callback(self->callbacks.on_open);
            }
        });
        channel->onClosed([weak_self] {
            if (const auto self = weak_self.lock()) {
                self->open.store(false);
                self->notify_closed();
            }
        });
        channel->onError([weak_self](std::string error) {
            if (const auto self = weak_self.lock()) self->report_error(std::move(error));
        });
        channel->onMessage(
            [weak_self](rtc::binary data) {
                if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                    std::vector<std::uint8_t> message(data.size());
                    if (!data.empty()) std::memcpy(message.data(), data.data(), data.size());
                    invoke_callback(self->callbacks.on_binary, std::move(message));
                }
            },
            [weak_self](std::string data) {
                if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                    invoke_callback(self->callbacks.on_text, std::move(data));
                }
            });

        if (channel->isOpen() && !closing.load()) {
            open.store(true);
            invoke_callback(callbacks.on_open);
        }
    }

    std::shared_ptr<rtc::DataChannel> current_channel() const {
        std::lock_guard lock(channel_mutex);
        return data_channel;
    }

    void report_error(std::string error) const noexcept {
        invoke_callback(callbacks.on_error, std::move(error));
    }

    void notify_closed() noexcept {
        if (!closed_notified.exchange(true)) invoke_callback(callbacks.on_closed);
    }

    void close() noexcept {
        if (closing.exchange(true)) return;
        open.store(false);

        try {
            if (const auto channel = current_channel()) {
                channel->resetCallbacks();
                channel->close();
            }
            if (peer_connection) {
                peer_connection->resetCallbacks();
                peer_connection->close();
            }
        } catch (...) {
        }

        connection_state.store(State::Closed);
        current_ice_state.store(IceState::Closed);
        notify_closed();
    }
};

WebRtcTransport::WebRtcTransport(Configuration configuration, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(std::move(configuration), std::move(callbacks))) {
    impl_->initialize();
}

WebRtcTransport::~WebRtcTransport() {
    close();
}

void WebRtcTransport::start() {
    impl_->start();
}

void WebRtcTransport::set_remote_description(std::string_view sdp, std::string_view type) {
    if (sdp.empty()) throw std::invalid_argument("Remote WebRTC SDP must not be empty");
    if (type.empty()) throw std::invalid_argument("Remote WebRTC SDP type must not be empty");
    if (impl_->closing.load()) throw std::logic_error("WebRTC transport is closed");
    impl_->peer_connection->setRemoteDescription(
        rtc::Description(std::string(sdp), std::string(type)));
}

void WebRtcTransport::add_remote_candidate(std::string_view candidate, std::string_view mid) {
    if (candidate.empty()) throw std::invalid_argument("Remote WebRTC ICE candidate must not be empty");
    if (impl_->closing.load()) throw std::logic_error("WebRTC transport is closed");
    impl_->peer_connection->addRemoteCandidate(
        rtc::Candidate(std::string(candidate), std::string(mid)));
}

bool WebRtcTransport::send_text(std::string_view message) noexcept {
    try {
        const auto channel = impl_->current_channel();
        if (!channel || !channel->isOpen()) return false;
        (void)channel->send(std::string(message));
        return true;
    } catch (const std::exception& error) {
        impl_->report_error(error.what());
        return false;
    }
}

bool WebRtcTransport::send_binary(std::span<const std::uint8_t> message) noexcept {
    try {
        const auto channel = impl_->current_channel();
        if (!channel || !channel->isOpen()) return false;
        (void)channel->send(
            reinterpret_cast<const rtc::byte*>(message.data()), message.size());
        return true;
    } catch (const std::exception& error) {
        impl_->report_error(error.what());
        return false;
    }
}

void WebRtcTransport::close() noexcept {
    if (impl_) impl_->close();
}

bool WebRtcTransport::is_open() const noexcept {
    return impl_ && impl_->open.load();
}

WebRtcTransport::State WebRtcTransport::state() const noexcept {
    return impl_ ? impl_->connection_state.load() : State::Closed;
}

WebRtcTransport::IceState WebRtcTransport::ice_state() const noexcept {
    return impl_ ? impl_->current_ice_state.load() : IceState::Closed;
}

WebRtcTransport::GatheringState WebRtcTransport::gathering_state() const noexcept {
    return impl_ ? impl_->current_gathering_state.load() : GatheringState::New;
}

std::size_t WebRtcTransport::buffered_amount() const noexcept {
    if (!impl_) return 0;
    try {
        const auto channel = impl_->current_channel();
        return channel ? channel->bufferedAmount() : 0;
    } catch (...) {
        return 0;
    }
}

WebRtcTransport::Statistics WebRtcTransport::statistics() const {
    Statistics result;
    if (!impl_ || !impl_->peer_connection) return result;

    result.bytes_sent = impl_->peer_connection->bytesSent();
    result.bytes_received = impl_->peer_connection->bytesReceived();
    result.round_trip_time = impl_->peer_connection->rtt();

    rtc::Candidate local;
    rtc::Candidate remote;
    if (impl_->peer_connection->getSelectedCandidatePair(&local, &remote)) {
        result.local_candidate = to_public_candidate(local);
        result.remote_candidate = to_public_candidate(remote);
    }
    return result;
}

} // namespace remoe
