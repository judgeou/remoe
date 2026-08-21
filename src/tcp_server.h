#pragma once

#include <WinSock2.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace remoe {

class WinsockRuntime {
public:
    WinsockRuntime();
    ~WinsockRuntime();
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

class TcpServer {
public:
    TcpServer(const std::string& bind_address, std::uint16_t port);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Returns INVALID_SOCKET on timeout; throws on an actual socket error.
    [[nodiscard]] SOCKET accept_client(std::string& peer, std::chrono::milliseconds timeout) const;

private:
    std::vector<SOCKET> listen_sockets_;
};

class TcpClient {
public:
    explicit TcpClient(SOCKET socket = INVALID_SOCKET) noexcept : socket_(socket) {}
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&& other) noexcept;
    TcpClient& operator=(TcpClient&& other) noexcept;

    [[nodiscard]] bool send_all(const void* data, std::size_t size) const;
    [[nodiscard]] bool receive_all(void* data, std::size_t size) const;
    [[nodiscard]] bool connected() const noexcept { return socket_ != INVALID_SOCKET; }
    void close() noexcept;

private:
    SOCKET socket_ = INVALID_SOCKET;
};

} // namespace remoe
