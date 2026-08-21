#include "tcp_server.h"

#include <WS2tcpip.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace remoe {
namespace {

std::runtime_error socket_error(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed, WSA error " +
                              std::to_string(WSAGetLastError()));
}

} // namespace

WinsockRuntime::WinsockRuntime() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw socket_error("WSAStartup");
    }
}

WinsockRuntime::~WinsockRuntime() { WSACleanup(); }

TcpServer::TcpServer(const std::string& bind_address, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* addresses = nullptr;
    const auto port_text = std::to_string(port);
    const bool wildcard = bind_address == "*" || bind_address == "0.0.0.0" || bind_address == "::";
    const bool loopback = bind_address == "localhost";
    const char* node = wildcard ? nullptr : (loopback ? "localhost" : bind_address.c_str());
    const int result = getaddrinfo(node, port_text.c_str(), &hints, &addresses);
    if (result != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerrorA(result)));
    }

    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        if (address->ai_family != AF_INET && address->ai_family != AF_INET6) continue;
        bool family_already_bound = false;
        for (SOCKET existing : listen_sockets_) {
            sockaddr_storage bound{};
            int bound_length = sizeof(bound);
            if (getsockname(existing, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0 &&
                bound.ss_family == address->ai_family) {
                family_already_bound = true;
                break;
            }
        }
        if (family_already_bound) continue;

        SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;

        BOOL exclusive = TRUE;
        setsockopt(candidate, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        if (address->ai_family == AF_INET6) {
            DWORD ipv6_only = TRUE;
            setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY,
                       reinterpret_cast<const char*>(&ipv6_only), sizeof(ipv6_only));
        }
        if (bind(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0 &&
            listen(candidate, 1) == 0) {
            listen_sockets_.push_back(candidate);
            continue;
        }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);

    if (listen_sockets_.empty()) throw socket_error("bind/listen");
    if ((wildcard || loopback) && listen_sockets_.size() != 2) {
        for (SOCKET socket : listen_sockets_) closesocket(socket);
        listen_sockets_.clear();
        throw std::runtime_error("failed to bind both IPv4 and IPv6 listening sockets");
    }
}

TcpServer::~TcpServer() {
    for (SOCKET socket : listen_sockets_) closesocket(socket);
}

SOCKET TcpServer::accept_client(std::string& peer, std::chrono::milliseconds timeout) const {
    fd_set readable;
    FD_ZERO(&readable);
    for (SOCKET socket : listen_sockets_) FD_SET(socket, &readable);
    timeval wait{};
    wait.tv_sec = static_cast<long>(timeout.count() / 1000);
    wait.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int ready = select(0, &readable, nullptr, nullptr, &wait);
    if (ready == 0) return INVALID_SOCKET;
    if (ready == SOCKET_ERROR) throw socket_error("select");

    sockaddr_storage address{};
    int address_length = sizeof(address);
    SOCKET ready_socket = INVALID_SOCKET;
    for (SOCKET socket : listen_sockets_) {
        if (FD_ISSET(socket, &readable)) {
            ready_socket = socket;
            break;
        }
    }
    if (ready_socket == INVALID_SOCKET) throw std::runtime_error("select returned without a readable socket");
    SOCKET client = accept(ready_socket, reinterpret_cast<sockaddr*>(&address), &address_length);
    if (client == INVALID_SOCKET) throw socket_error("accept");

    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> service{};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&address), address_length,
                    host.data(), static_cast<DWORD>(host.size()),
                    service.data(), static_cast<DWORD>(service.size()),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        peer = std::string(host.data()) + ":" + service.data();
    } else {
        peer = "unknown";
    }

    BOOL no_delay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    DWORD send_timeout_ms = 2000;
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_timeout_ms), sizeof(send_timeout_ms));
    DWORD receive_timeout_ms = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&receive_timeout_ms), sizeof(receive_timeout_ms));
    return client;
}

TcpClient::~TcpClient() { close(); }

TcpClient::TcpClient(TcpClient&& other) noexcept : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {}

TcpClient& TcpClient::operator=(TcpClient&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, INVALID_SOCKET);
    }
    return *this;
}

bool TcpClient::send_all(const void* data, std::size_t size) const {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const int chunk = static_cast<int>((std::min)(size,
            static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int sent = send(socket_, bytes, chunk, 0);
        if (sent == SOCKET_ERROR || sent == 0) return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool TcpClient::receive_all(void* data, std::size_t size,
                            const std::atomic_bool* running) const {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
        if (running && !*running) return false;
        const int chunk = static_cast<int>((std::min)(size,
            static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int received = recv(socket_, bytes, chunk, 0);
        if (received == 0) return false;
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (running && (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK)) continue;
            return false;
        }
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

void TcpClient::close() noexcept {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

} // namespace remoe
