#include "webrtc_websocket_signaling.h"

#include "webrtc_tcp_bootstrap.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace remoe {
namespace {

constexpr std::size_t kMaxBufferedSignalingBytes = 2 * 1024 * 1024;

std::string windows_root_ca_bundle() {
    std::string bundle;
    constexpr DWORD locations[] = {
        CERT_SYSTEM_STORE_CURRENT_USER,
        CERT_SYSTEM_STORE_LOCAL_MACHINE,
    };
    for (const DWORD location : locations) {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                         location | CERT_STORE_READONLY_FLAG, L"ROOT");
        if (!store) continue;
        PCCERT_CONTEXT certificate = nullptr;
        while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
            DWORD output_size = 0;
            if (!CryptBinaryToStringA(certificate->pbCertEncoded,
                                      certificate->cbCertEncoded,
                                      CRYPT_STRING_BASE64HEADER,
                                      nullptr, &output_size) || output_size == 0) {
                continue;
            }
            const std::size_t offset = bundle.size();
            bundle.resize(offset + output_size);
            if (!CryptBinaryToStringA(certificate->pbCertEncoded,
                                      certificate->cbCertEncoded,
                                      CRYPT_STRING_BASE64HEADER,
                                      bundle.data() + offset, &output_size)) {
                bundle.resize(offset);
                continue;
            }
            // CryptBinaryToString includes a trailing NUL; concatenated PEM does not.
            bundle.resize(offset + output_size - 1);
        }
        CertCloseStore(store, 0);
    }
    if (bundle.empty()) {
        throw std::runtime_error("Windows root CA certificate store is empty");
    }
    return bundle;
}

bool valid_session_id(std::string_view value) {
    if (value.size() < 8 || value.size() > 64) return false;
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

bool valid_url_token(std::string_view value, std::size_t minimum = 16) {
    if (value.size() < minimum || value.size() > 128) return false;
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

std::string host_endpoint(std::string signaling_url) {
    if (signaling_url.find('#') != std::string::npos) {
        throw std::invalid_argument("Managed Host signaling URL must not contain a fragment");
    }
    const std::size_t scheme = signaling_url.find("://");
    if (scheme == std::string::npos ||
        (!signaling_url.starts_with("ws://") && !signaling_url.starts_with("wss://"))) {
        throw std::invalid_argument("WebRTC signaling URL must use ws:// or wss://");
    }
    const std::size_t path = signaling_url.find_first_of("/?", scheme + 3);
    if (path != std::string::npos) signaling_url.resize(path);
    signaling_url += "/host";
    return signaling_url;
}

class WebSocketSignalingStream final
    : public std::enable_shared_from_this<WebSocketSignalingStream> {
public:
    ~WebSocketSignalingStream() { close(); }

    void connect(const std::string& url, std::chrono::milliseconds timeout,
                 std::function<bool()> stop_requested, bool client_role,
                 std::vector<std::string> protocols = {}) {
        rtc::WebSocket::Configuration configuration;
        configuration.connectionTimeout = timeout;
        configuration.maxMessageSize = 1024 * 1024 + 1024;
        configuration.protocols = std::move(protocols);
        if (url.starts_with("wss://")) {
            static const std::string root_ca_bundle = windows_root_ca_bundle();
            configuration.caCertificatePemFile = root_ca_bundle;
        }
        stop_requested_ = std::move(stop_requested);
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
            [weak_self](std::string message) {
                if (const auto self = weak_self.lock()) {
                    if (message == "registered") {
                        {
                            std::lock_guard lock(self->mutex_);
                            self->registered_ = true;
                        }
                        self->changed_.notify_all();
                    } else if (message == "error:invite-not-found") {
                        self->set_error("Invite not found or expired");
                    } else if (message == "error:invite-in-use") {
                        self->set_error("Invite is already in use");
                    } else if (message == "error:service-unavailable") {
                        self->set_error("Signaling server has reached its session limit");
                    } else if (message == "error:host-in-use") {
                        self->set_error("Host invite is already registered");
                    } else {
                        self->set_error("Signaling server sent an unknown registration response");
                    }
                }
            });
        websocket_->open(url);

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock(mutex_);
        changed_.wait_until(lock, deadline, [&] {
            return registered_ || closed_ || !error_.empty();
        });
        if (!registered_) {
            std::string detail = error_.empty() ? "connection timed out" : error_;
            if (client_role && (detail == "Invite not found or expired" ||
                                detail == "Invite is already in use")) {
                throw std::runtime_error(detail);
            }
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
            while (incoming_.size() < size && !closed_ && error_.empty()) {
                if (stop_requested_ && stop_requested_()) return false;
                const auto poll_deadline = (std::min)(
                    deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
                changed_.wait_until(lock, poll_deadline);
                if (std::chrono::steady_clock::now() >= deadline &&
                    incoming_.size() < size) return false;
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
    std::function<bool()> stop_requested_;
    bool open_ = false;
    bool registered_ = false;
    bool closed_ = false;
};

} // namespace

bool valid_managed_host_identity(const ManagedHostIdentity& identity) noexcept {
    return valid_url_token(identity.device_id) && valid_url_token(identity.token, 32);
}

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

ManagedHostIdentity pair_managed_webrtc_host(
    std::string signaling_url, std::optional<ManagedHostIdentity> current_identity,
    std::function<void(std::string)> on_pairing_code,
    std::function<bool()> stop_requested) {
    if (current_identity && !valid_managed_host_identity(*current_identity)) {
        throw std::invalid_argument("The existing managed Host identity is invalid");
    }
    const std::string endpoint = host_endpoint(std::move(signaling_url));
    std::string protocol = "remoe-pair.new";
    if (current_identity) {
        protocol = "remoe-pair." + current_identity->device_id + '.' + current_identity->token;
    }

    rtc::WebSocket::Configuration configuration;
    configuration.connectionTimeout = std::chrono::seconds(15);
    configuration.maxMessageSize = 4096;
    configuration.protocols = {std::move(protocol)};
    if (endpoint.starts_with("wss://")) {
        static const std::string root_ca_bundle = windows_root_ca_bundle();
        configuration.caCertificatePemFile = root_ca_bundle;
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::optional<ManagedHostIdentity> result;
    std::string error;
    bool closed = false;
    auto websocket = std::make_shared<rtc::WebSocket>(std::move(configuration));
    websocket->onMessage(
        [](rtc::binary) {},
        [&](std::string message) {
            if (message.starts_with("pairing:")) {
                if (on_pairing_code) on_pairing_code(message.substr(8));
                return;
            }
            if (message.starts_with("paired:")) {
                const std::size_t separator = message.find(':', 7);
                if (separator != std::string::npos) {
                    ManagedHostIdentity identity{
                        message.substr(7, separator - 7), message.substr(separator + 1)};
                    std::lock_guard lock(mutex);
                    if (valid_managed_host_identity(identity)) result = std::move(identity);
                    else error = "Pairing server returned an invalid Host identity";
                    changed.notify_all();
                    return;
                }
            }
            std::lock_guard lock(mutex);
            error = "Pairing server returned an unknown response";
            changed.notify_all();
        });
    websocket->onClosed([&] {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    });
    websocket->onError([&](std::string detail) {
        std::lock_guard lock(mutex);
        error = std::move(detail);
        changed.notify_all();
    });
    websocket->open(endpoint);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(11);
    std::unique_lock lock(mutex);
    while (!result && error.empty() && !closed && std::chrono::steady_clock::now() < deadline) {
        if (stop_requested && stop_requested()) {
            error = "Pairing cancelled";
            break;
        }
        changed.wait_for(lock, std::chrono::milliseconds(250));
    }
    lock.unlock();
    websocket->resetCallbacks();
    websocket->close();
    if (result) return *result;
    if (error.empty()) error = closed ? "Pairing connection closed" : "Pairing code expired";
    throw std::runtime_error(error);
}

std::unique_ptr<WebRtcTransport> establish_webrtc_over_websocket(
    WebRtcTransport::Role role, std::string signaling_invite_url,
    WebRtcTransport::Callbacks callbacks, std::chrono::milliseconds timeout,
    std::function<bool()> stop_requested, std::function<void()> on_signaling_open) {
    auto stream = std::make_shared<WebSocketSignalingStream>();
    auto [signaling_url, session_id] = split_invite(std::move(signaling_invite_url));
    const std::string stun_url = derive_stun_url(signaling_url);
    const std::string url = make_url(std::move(signaling_url), session_id, role);
    constexpr auto connection_timeout = std::chrono::seconds(15);
    stream->connect(url, timeout == (std::chrono::milliseconds::max)()
                             ? connection_timeout : timeout,
                    std::move(stop_requested), role == WebRtcTransport::Role::Offerer);
    if (on_signaling_open) on_signaling_open();

    WebRtcTcpBootstrapIo io;
    io.send_all = [stream](const void* data, std::size_t size) {
        return stream->send_all(data, size);
    };
    io.receive_all = [stream](void* data, std::size_t size, auto deadline) {
        return stream->receive_all(data, size, deadline);
    };
    return establish_webrtc_over_tcp(
        role, std::move(io), std::move(callbacks), timeout, {stun_url}, true);
}

std::unique_ptr<WebRtcTransport> establish_managed_host_webrtc(
    std::string signaling_url, const ManagedHostIdentity& identity,
    WebRtcTransport::Callbacks callbacks, std::chrono::milliseconds timeout,
    std::function<bool()> stop_requested, std::function<void()> on_signaling_open) {
    if (!valid_managed_host_identity(identity)) {
        throw std::invalid_argument("Managed Host identity is invalid; run with --repair");
    }
    const std::string stun_url = derive_stun_url(signaling_url);
    const std::string endpoint = host_endpoint(std::move(signaling_url));
    const std::string protocol =
        "remoe-host." + identity.device_id + '.' + identity.token;
    auto stream = std::make_shared<WebSocketSignalingStream>();
    constexpr auto connection_timeout = std::chrono::seconds(15);
    stream->connect(endpoint, timeout == (std::chrono::milliseconds::max)()
                                  ? connection_timeout : timeout,
                    std::move(stop_requested), false, {protocol});
    if (on_signaling_open) on_signaling_open();

    WebRtcTcpBootstrapIo io;
    io.send_all = [stream](const void* data, std::size_t size) {
        return stream->send_all(data, size);
    };
    io.receive_all = [stream](void* data, std::size_t size, auto deadline) {
        return stream->receive_all(data, size, deadline);
    };
    return establish_webrtc_over_tcp(
        WebRtcTransport::Role::Answerer, std::move(io), std::move(callbacks),
        timeout, {stun_url}, true);
}

} // namespace remoe
