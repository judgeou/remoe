#include "protocol.h"
#include "video_window.h"
#include "vpl_decoder.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 47990;
    std::uint32_t fps = 60;
    std::uint32_t bitrate_mbps = 20;
};

std::uint32_t parse_u32(std::string_view text, std::string_view name,
                        std::uint32_t minimum, std::uint32_t maximum) {
    std::size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(std::string(text), &used, 10);
    } catch (...) {
        throw std::runtime_error("invalid value for " + std::string(name));
    }
    if (used != text.size() || value < minimum || value > maximum) {
        throw std::runtime_error("value for " + std::string(name) + " is out of range");
    }
    return static_cast<std::uint32_t>(value);
}

void print_help() {
    std::cout <<
        "remoe_client - low-power Intel AV1 remote desktop viewer\n\n"
        "Usage: remoe_client [options]\n"
        "  --host <address>  Host address (default: 127.0.0.1)\n"
        "  --port <1-65535>  TCP port (default: 47990)\n"
        "  --fps <1-240>     Requested frame rate (default: 60)\n"
        "  --bitrate <Mbps>  Requested AV1 bitrate/quality (default: 20)\n"
        "  --help            Show this help\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            print_help();
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value after " + std::string(argument));
        const std::string_view value(argv[++i]);
        if (argument == "--host") options.host = value;
        else if (argument == "--port") {
            options.port = static_cast<std::uint16_t>(parse_u32(value, argument, 1, 65535));
        } else if (argument == "--fps") {
            options.fps = parse_u32(value, argument, 1, 240);
        } else if (argument == "--bitrate") {
            options.bitrate_mbps = parse_u32(value, argument, 1, 1000);
        }
        else throw std::runtime_error("unknown option: " + std::string(argument));
    }
    return options;
}

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int status = WSAStartup(MAKEWORD(2, 2), &data);
        if (status != 0) throw std::runtime_error("WSAStartup failed, error " + std::to_string(status));
    }
    ~WinsockRuntime() { WSACleanup(); }
};

class StreamConnection {
public:
    StreamConnection(const std::string& host, std::uint16_t port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const std::string service = std::to_string(port);
        const int status = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
        if (status != 0) {
            throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerrorA(status)));
        }
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == INVALID_SOCKET) continue;
            if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
                socket_ = candidate;
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addresses);
        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error("cannot connect to " + host + ":" + service +
                                     ", WSA error " + std::to_string(WSAGetLastError()));
        }
        BOOL no_delay = TRUE;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        DWORD timeout_ms = 250;
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    }

    ~StreamConnection() {
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
        }
    }
    StreamConnection(const StreamConnection&) = delete;
    StreamConnection& operator=(const StreamConnection&) = delete;

    bool receive_all(void* data, std::size_t size, const std::atomic_bool* running = nullptr) {
        char* output = static_cast<char*>(data);
        while (size > 0) {
            if (running && !*running) return false;
            const int chunk = static_cast<int>((std::min)(
                size, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
            const int received = recv(socket_, output, chunk, 0);
            if (received > 0) {
                output += received;
                size -= static_cast<std::size_t>(received);
                continue;
            }
            if (received == 0) return false;
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;
            return false;
        }
        return true;
    }

    bool send_all(const void* data, std::size_t size) {
        const char* input = static_cast<const char*>(data);
        while (size > 0) {
            const int chunk = static_cast<int>((std::min)(
                size, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
            const int sent = send(socket_, input, chunk, 0);
            if (sent == SOCKET_ERROR || sent == 0) return false;
            input += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

void validate_stream_header(const remoe::protocol::StreamHeader& header) {
    if (header.magic != remoe::protocol::kStreamMagic ||
        header.version != remoe::protocol::kVersion ||
        header.header_size != sizeof(header)) {
        throw std::runtime_error("host sent an incompatible stream header");
    }
    if (header.codec != remoe::protocol::kCodecAv1) {
        throw std::runtime_error("host stream is not AV1");
    }
    if (header.width == 0 || header.height == 0 || header.width > 16384 || header.height > 16384) {
        throw std::runtime_error("host sent an invalid video resolution");
    }
}

void receive_stream(StreamConnection& connection, remoe::VideoWindow& window,
                    std::exception_ptr& worker_error) {
    try {
        remoe::VplAv1Decoder decoder(window.device(),
            [&window](ID3D11Texture2D* texture, std::uint32_t width, std::uint32_t height) {
                window.present(texture, width, height);
            });
        std::cout << "Decoder: " << decoder.implementation_name() << '\n';

        std::vector<std::uint8_t> payload;
        while (window.running()) {
            remoe::protocol::FrameHeader header;
            if (!connection.receive_all(&header, sizeof(header), window.running_flag())) {
                if (!window.running()) break;
                throw std::runtime_error("host disconnected");
            }
            if (header.magic != remoe::protocol::kFrameMagic ||
                header.version != remoe::protocol::kVersion ||
                header.header_size != sizeof(header)) {
                throw std::runtime_error("host sent an incompatible frame header");
            }
            constexpr std::uint32_t max_payload = 64 * 1024 * 1024;
            if (header.payload_size == 0 || header.payload_size > max_payload) {
                throw std::runtime_error("host sent an invalid AV1 payload size");
            }
            payload.resize(header.payload_size);
            if (!connection.receive_all(payload.data(), payload.size(), window.running_flag())) {
                if (!window.running()) break;
                throw std::runtime_error("host disconnected during an AV1 frame");
            }
            decoder.submit(payload);
        }
        decoder.drain();
    } catch (...) {
        worker_error = std::current_exception();
        window.request_close();
    }
}

int run(const Options& options) {
    WinsockRuntime winsock;
    std::cout << "Connecting to " << options.host << ':' << options.port << "...\n";
    StreamConnection connection(options.host, options.port);

    remoe::protocol::ClientConfig request;
    request.fps_num = options.fps;
    request.bitrate_bps = options.bitrate_mbps * 1'000'000u;
    if (!connection.send_all(&request, sizeof(request))) {
        throw std::runtime_error("failed to send the protocol v2 stream request");
    }

    remoe::protocol::StreamHeader stream_header;
    if (!connection.receive_all(&stream_header, sizeof(stream_header))) {
        throw std::runtime_error("connection closed before the stream header");
    }
    validate_stream_header(stream_header);
    if (stream_header.fps_num != request.fps_num ||
        stream_header.fps_den != request.fps_den ||
        stream_header.bitrate_bps != request.bitrate_bps) {
        throw std::runtime_error("host did not honor the requested frame rate and bitrate");
    }
    std::cout << "Stream: " << stream_header.width << 'x' << stream_header.height << ", "
              << stream_header.fps_num << '/' << stream_header.fps_den << " fps, "
              << stream_header.bitrate_bps / 1'000'000.0 << " Mbps\n";

    remoe::VideoWindow window(stream_header.width, stream_header.height);
    std::exception_ptr worker_error;
    std::thread worker(receive_stream, std::ref(connection), std::ref(window),
                       std::ref(worker_error));
    const int result = window.message_loop();
    window.stop();
    worker.join();
    if (worker_error) std::rethrow_exception(worker_error);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
