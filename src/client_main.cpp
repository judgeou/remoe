#include "protocol.h"
#include "video_window.h"
#include "vpl_decoder.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
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
    std::uint32_t scale_percent = 100;
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
        "  --scale <10-100>  Requested encoding resolution percent (default: 100)\n"
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
        } else if (argument == "--scale") {
            options.scale_percent = parse_u32(value, argument, 10, 100);
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
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
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
        std::lock_guard lock(send_mutex_);
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
    std::mutex send_mutex_;
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

struct EncodedFrame {
    remoe::protocol::FrameHeader header;
    std::vector<std::uint8_t> payload;
    bool reset_decoder = false;
};

class EncodedFrameQueue {
public:
    enum class PushResult {
        Queued,
        Dropped,
        RequestKeyFrame,
        Stopped,
    };

    EncodedFrameQueue(std::size_t max_bytes, std::size_t max_frames)
        : max_bytes_(max_bytes), max_frames_(max_frames) {}

    PushResult push(EncodedFrame frame) {
        std::lock_guard lock(mutex_);
        if (stopped_) return PushResult::Stopped;

        const bool key_frame = (frame.header.flags & remoe::protocol::kFrameKey) != 0;
        if (waiting_for_key_frame_) {
            if (!key_frame) {
                ++dropped_frames_;
                return PushResult::Dropped;
            }
            frame.reset_decoder = true;
            waiting_for_key_frame_ = false;
            std::cerr << "Decoder queue recovered at key frame "
                      << frame.header.frame_number << " after dropping "
                      << dropped_frames_ << " frames\n";
            dropped_frames_ = 0;
        }

        if (!frames_.empty() &&
            (queued_bytes_ + frame.payload.size() > max_bytes_ ||
             frames_.size() >= max_frames_)) {
            dropped_frames_ += frames_.size();
            frames_.clear();
            queued_bytes_ = 0;
            waiting_for_key_frame_ = true;
            std::cerr << "Decoder queue overflow; dropping old GOP and waiting for a key frame\n";
            if (!key_frame) {
                ++dropped_frames_;
                return PushResult::RequestKeyFrame;
            }
            frame.reset_decoder = true;
            waiting_for_key_frame_ = false;
            dropped_frames_ = 0;
        }

        queued_bytes_ += frame.payload.size();
        frames_.push_back(std::move(frame));
        condition_.notify_one();
        return PushResult::Queued;
    }

    bool pop(EncodedFrame& frame, const std::atomic_bool& running) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return stopped_ || !frames_.empty() || !running; });
        if (!running || frames_.empty()) return false;
        frame = std::move(frames_.front());
        queued_bytes_ -= frame.payload.size();
        frames_.pop_front();
        return true;
    }

    void stop() {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<EncodedFrame> frames_;
    std::size_t queued_bytes_ = 0;
    std::size_t dropped_frames_ = 0;
    const std::size_t max_bytes_;
    const std::size_t max_frames_;
    bool waiting_for_key_frame_ = false;
    bool stopped_ = false;
};

class PipelineState {
public:
    void fail(std::exception_ptr error, EncodedFrameQueue& queue,
              remoe::VideoWindow& window) {
        {
            std::lock_guard lock(mutex_);
            if (!error_) error_ = std::move(error);
        }
        queue.stop();
        window.request_close();
    }

    void rethrow_if_failed() {
        std::exception_ptr error;
        {
            std::lock_guard lock(mutex_);
            error = error_;
        }
        if (error) std::rethrow_exception(error);
    }

private:
    std::mutex mutex_;
    std::exception_ptr error_;
};

void receive_frames(StreamConnection& connection, remoe::VideoWindow& window,
                    EncodedFrameQueue& queue, PipelineState& pipeline) {
    try {
        auto statistics_epoch = std::chrono::steady_clock::now();
        std::uint64_t video_bytes = 0;
        std::uint64_t network_bytes = 0;
        while (window.running()) {
            EncodedFrame frame;
            if (!connection.receive_all(&frame.header, sizeof(frame.header),
                                        window.running_flag())) {
                if (!window.running()) break;
                throw std::runtime_error("host disconnected");
            }
            if (frame.header.magic != remoe::protocol::kFrameMagic ||
                frame.header.version != remoe::protocol::kVersion ||
                frame.header.header_size != sizeof(frame.header)) {
                throw std::runtime_error("host sent an incompatible frame header");
            }
            constexpr std::uint32_t max_payload = 64 * 1024 * 1024;
            if (frame.header.payload_size == 0 || frame.header.payload_size > max_payload) {
                throw std::runtime_error("host sent an invalid AV1 payload size");
            }
            frame.payload.resize(frame.header.payload_size);
            if (!connection.receive_all(frame.payload.data(), frame.payload.size(),
                                        window.running_flag())) {
                if (!window.running()) break;
                throw std::runtime_error("host disconnected during an AV1 frame");
            }
            video_bytes += frame.payload.size();
            network_bytes += sizeof(frame.header) + frame.payload.size();
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - statistics_epoch).count();
            if (elapsed >= 1.0) {
                window.update_transfer_statistics(
                    static_cast<double>(video_bytes) * 8.0 / elapsed / 1'000'000.0,
                    static_cast<double>(network_bytes) / elapsed / 1'000'000.0);
                statistics_epoch = now;
                video_bytes = 0;
                network_bytes = 0;
            }
            const auto push_result = queue.push(std::move(frame));
            if (push_result == EncodedFrameQueue::PushResult::Stopped) break;
            if (push_result == EncodedFrameQueue::PushResult::RequestKeyFrame) {
                remoe::protocol::InputEvent request;
                request.type = remoe::protocol::InputType::RequestKeyFrame;
                if (!connection.send_all(&request, sizeof(request))) {
                    throw std::runtime_error("failed to request an AV1 key frame");
                }
                std::cerr << "Requested an immediate key frame from the host\n";
            }
        }
        queue.stop();
    } catch (...) {
        pipeline.fail(std::current_exception(), queue, window);
    }
}

void decode_frames(remoe::VideoWindow& window, EncodedFrameQueue& queue,
                   PipelineState& pipeline) {
    try {
        const auto create_decoder = [&window] {
            return std::make_unique<remoe::VplAv1Decoder>(window.device(),
                [&window](ID3D11Texture2D* texture, std::uint32_t width,
                          std::uint32_t height) {
                    window.present(texture, width, height);
                });
        };
        auto decoder = create_decoder();
        std::cout << "Decoder: " << decoder->implementation_name() << '\n';

        EncodedFrame frame;
        while (queue.pop(frame, *window.running_flag())) {
            if (frame.reset_decoder) {
                decoder.reset();
                decoder = create_decoder();
                std::cerr << "Decoder reset after dropping stale frames\n";
            }
            decoder->submit(frame.payload);
        }
        if (window.running()) decoder->drain();
    } catch (...) {
        pipeline.fail(std::current_exception(), queue, window);
    }
}

int run(const Options& options) {
    WinsockRuntime winsock;
    std::cout << "Connecting to " << options.host << ':' << options.port << "...\n";
    StreamConnection connection(options.host, options.port);

    remoe::protocol::ClientConfig request;
    request.fps_num = options.fps;
    request.bitrate_bps = options.bitrate_mbps * 1'000'000u;
    request.scale_percent = options.scale_percent;
    if (!connection.send_all(&request, sizeof(request))) {
        throw std::runtime_error("failed to send the protocol v5 stream request");
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
    window.set_input_callback([&connection](const remoe::protocol::InputEvent& event) {
        return connection.send_all(&event, sizeof(event));
    });
    constexpr std::size_t minimum_queue_bytes = 8 * 1024 * 1024;
    constexpr std::size_t maximum_queue_bytes = 64 * 1024 * 1024;
    const std::size_t two_seconds_of_stream =
        static_cast<std::size_t>(stream_header.bitrate_bps) / 8 * 2;
    const std::size_t queue_bytes = (std::clamp)(two_seconds_of_stream,
                                                 minimum_queue_bytes,
                                                 maximum_queue_bytes);
    const std::size_t queue_frames = static_cast<std::size_t>(stream_header.fps_num) * 2;
    EncodedFrameQueue queue(queue_bytes, queue_frames);
    PipelineState pipeline;
    std::thread receiver(receive_frames, std::ref(connection), std::ref(window),
                         std::ref(queue), std::ref(pipeline));
    std::thread decoder(decode_frames, std::ref(window), std::ref(queue),
                        std::ref(pipeline));
    const int result = window.message_loop();
    window.stop();
    queue.stop();
    receiver.join();
    decoder.join();
    pipeline.rethrow_if_failed();
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
