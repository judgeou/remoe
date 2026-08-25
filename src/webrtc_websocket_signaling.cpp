#include "webrtc_websocket_signaling.h"

#include "webrtc_tcp_bootstrap.h"

#include <Windows.h>
#include <bcrypt.h>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace remoe {
namespace {

constexpr std::size_t kMaxBufferedSignalingBytes = 2 * 1024 * 1024;

bool valid_session_id(std::string_view value) {
    if (value.size() < 8 || value.size() > 64) return false;
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

std::string make_url(std::string base, std::string_view session,
                     WebRtcTransport::Role role) {
    if (!base.starts_with("ws://") && !base.starts_with("wss://")) {
        throw std::invalid_argument("WebRTC signaling URL must use ws:// or wss://");
    }
    if (base.find('#') != std::string::npos) {
        throw std::invalid_argument("WebRTC signaling URL must not contain a fragment");
    }
    if (!valid_session_id(session)) {
        throw std::invalid_argument(
            "WebRTC session ID must be 8-64 letters, digits, '_' or '-'");
    }
    base += base.find('?') == std::string::npos ? '?' : '&';
    base += "session=";
    base += session;
    base += "&role=";
    base += role == WebRtcTransport::Role::Offerer ? "client" : "host";
    return base;
}

std::pair<std::string, std::string> split_invite(std::string invite) {
    const std::size_t fragment = invite.find('#');
    if (fragment == std::string::npos) {
        throw std::invalid_argument(
            "WebRTC signaling invite URL must contain a session ID after '#'");
    }
    std::string session = invite.substr(fragment + 1);
    invite.resize(fragment);
    if (!valid_session_id(session)) {
        throw std::invalid_argument(
            "WebRTC signaling invite has an invalid session ID");
    }
    return {std::move(invite), std::move(session)};
}

std::string derive_stun_url(std::string_view signaling_url) {
    constexpr std::string_view ws_scheme = "ws://";
    constexpr std::string_view wss_scheme = "wss://";
    std::size_t authority_begin = 0;
    if (signaling_url.starts_with(wss_scheme)) authority_begin = wss_scheme.size();
    else if (signaling_url.starts_with(ws_scheme)) authority_begin = ws_scheme.size();
    else throw std::invalid_argument("WebRTC signaling URL must use ws:// or wss://");

    const std::size_t authority_end = signaling_url.find_first_of("/?#", authority_begin);
    std::string_view authority = signaling_url.substr(
        authority_begin, authority_end == std::string_view::npos
            ? std::string_view::npos : authority_end - authority_begin);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("WebRTC signaling URL has an invalid authority");
    }

    std::string_view host;
    if (authority.front() == '[') {
        const std::size_t closing_bracket = authority.find(']');
        if (closing_bracket == std::string_view::npos || closing_bracket == 1 ||
            (closing_bracket + 1 < authority.size() && authority[closing_bracket + 1] != ':')) {
            throw std::invalid_argument("WebRTC signaling URL has an invalid IPv6 host");
        }
        host = authority.substr(0, closing_bracket + 1);
    } else {
        const std::size_t port_separator = authority.rfind(':');
        host = port_separator == std::string_view::npos
            ? authority : authority.substr(0, port_separator);
        if (host.empty() || host.find(':') != std::string_view::npos) {
            throw std::invalid_argument("WebRTC signaling URL has an invalid host");
        }
    }
    return "stun:" + std::string(host) + ":3478";
}

class WebSocketSignalingStream final
    : public std::enable_shared_from_this<WebSocketSignalingStream> {
public:
    ~WebSocketSignalingStream() { close(); }

    void connect(const std::string& url, std::chrono::milliseconds timeout) {
        rtc::WebSocket::Configuration configuration;
        configuration.connectionTimeout = timeout;
        configuration.maxMessageSize = 1024 * 1024 + 1024;
        websocket_ = std::make_shared<rtc::WebSocket>(std::move(configuration));
        const std::weak_ptr<WebSocketSignalingStream> weak_self = weak_from_this();
        websocket_->onOpen([weak_self] {
            if (const auto self = weak_self.lock()) {
                {
                    std::lock_guard lock(self->mutex_);
                    self->open_ = true;
                }
                self->changed_.notify_all();
            }
        });
        websocket_->onClosed([weak_self] {
            if (const auto self = weak_self.lock()) {
                {
                    std::lock_guard lock(self->mutex_);
                    self->closed_ = true;
                }
                self->changed_.notify_all();
            }
        });
        websocket_->onError([weak_self](std::string error) {
            if (const auto self = weak_self.lock()) self->set_error(std::move(error));
        });
        websocket_->onMessage(
            [weak_self](rtc::binary message) {
                if (const auto self = weak_self.lock()) self->receive_binary(std::move(message));
            },
            [weak_self](std::string) {
                if (const auto self = weak_self.lock()) {
                    self->set_error("Signaling server sent a text WebSocket message");
                }
            });
        websocket_->open(url);

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, deadline, [&] { return open_ || closed_ || !error_.empty(); });
        if (!open_) {
            const std::string detail = error_.empty() ? "connection timed out" : error_;
            throw std::runtime_error("WebSocket signaling connection failed: " + detail);
        }
    }

    bool send_all(const void* data, std::size_t size) noexcept {
        try {
            std::shared_ptr<rtc::WebSocket> websocket;
            {
                std::lock_guard lock(mutex_);
                if (!open_ || closed_ || !error_.empty()) return false;
                websocket = websocket_;
            }
            rtc::binary message(size);
            if (size != 0) std::memcpy(message.data(), data, size);
            std::lock_guard send_lock(send_mutex_);
            (void)websocket->send(std::move(message));
            return true;
        } catch (const std::exception& error) {
            set_error(error.what());
            return false;
        }
    }

    bool receive_all(void* data, std::size_t size,
                     std::chrono::steady_clock::time_point deadline) noexcept {
        try {
            auto* output = static_cast<std::uint8_t*>(data);
            std::unique_lock lock(mutex_);
            if (!changed_.wait_until(lock, deadline, [&] {
                    return incoming_.size() >= size || closed_ || !error_.empty();
                })) {
                return false;
            }
            if (incoming_.size() < size) return false;
            for (std::size_t index = 0; index < size; ++index) {
                output[index] = incoming_.front();
                incoming_.pop_front();
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void close() noexcept {
        std::shared_ptr<rtc::WebSocket> websocket;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return;
            closed_ = true;
            websocket = websocket_;
        }
        changed_.notify_all();
        try {
            if (websocket) {
                websocket->resetCallbacks();
                websocket->close();
            }
        } catch (...) {
        }
    }

private:
    void receive_binary(rtc::binary message) noexcept {
        std::lock_guard lock(mutex_);
        if (incoming_.size() + message.size() > kMaxBufferedSignalingBytes) {
            error_ = "Signaling receive buffer limit exceeded";
        } else {
            if (!message.empty()) {
                const auto* begin = reinterpret_cast<const std::uint8_t*>(message.data());
                incoming_.insert(incoming_.end(), begin, begin + message.size());
            }
        }
        changed_.notify_all();
    }

    void set_error(std::string error) noexcept {
        {
            std::lock_guard lock(mutex_);
            if (error_.empty()) error_ = std::move(error);
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::mutex send_mutex_;
    std::condition_variable changed_;
    std::shared_ptr<rtc::WebSocket> websocket_;
    std::deque<std::uint8_t> incoming_;
    std::string error_;
    bool open_ = false;
    bool closed_ = false;
};

} // namespace

std::string create_webrtc_signaling_invite(std::string signaling_url) {
    if (!signaling_url.starts_with("ws://") && !signaling_url.starts_with("wss://")) {
        throw std::invalid_argument("WebRTC signaling URL must use ws:// or wss://");
    }
    if (signaling_url.find('#') != std::string::npos) {
        throw std::invalid_argument("WebRTC signaling base URL must not contain a fragment");
    }

    constexpr char alphabet[] = "_-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static_assert(sizeof(alphabet) - 1 == 64);
    std::array<UCHAR, 21> random_bytes{};
    if (BCryptGenRandom(nullptr, random_bytes.data(),
                        static_cast<ULONG>(random_bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("Failed to generate a secure WebRTC session ID");
    }
    signaling_url += '#';
    for (const UCHAR byte : random_bytes) {
        signaling_url += alphabet[byte & 0x3f];
    }
    return signaling_url;
}

std::unique_ptr<WebRtcTransport> establish_webrtc_over_websocket(
    WebRtcTransport::Role role, std::string signaling_invite_url,
    WebRtcTransport::Callbacks callbacks, std::chrono::milliseconds timeout) {
    auto stream = std::make_shared<WebSocketSignalingStream>();
    auto [signaling_url, session_id] = split_invite(std::move(signaling_invite_url));
    const std::string stun_url = derive_stun_url(signaling_url);
    const std::string url = make_url(std::move(signaling_url), session_id, role);
    stream->connect(url, timeout);

    WebRtcTcpBootstrapIo io;
    io.send_all = [stream](const void* data, std::size_t size) {
        return stream->send_all(data, size);
    };
    io.receive_all = [stream](void* data, std::size_t size, auto deadline) {
        return stream->receive_all(data, size, deadline);
    };
    return establish_webrtc_over_tcp(
        role, std::move(io), std::move(callbacks), timeout, {stun_url});
}

} // namespace remoe
