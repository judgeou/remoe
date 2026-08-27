#include "clipboard.h"
#include "protocol.h"
#include "video_window.h"
#include "vpl_decoder.h"
#include "webrtc_websocket_signaling.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::uint32_t fps = 60;
    std::uint32_t bitrate_mbps = 20;
    std::uint32_t scale_percent = 100;
    std::string signaling_url;
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
        "  --fps <1-240>     Requested frame rate (default: 60)\n"
        "  --bitrate <Mbps>  Requested AV1 bitrate/quality (default: 20)\n"
        "  --scale <10-100>  Requested encoding resolution percent (default: 100)\n"
        "  --signal-url <invite-url> Host invite URL (required)\n"
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
        if (argument == "--fps") {
            options.fps = parse_u32(value, argument, 1, 240);
        } else if (argument == "--bitrate") {
            options.bitrate_mbps = parse_u32(value, argument, 1, 1000);
        } else if (argument == "--scale") {
            options.scale_percent = parse_u32(value, argument, 10, 100);
        } else if (argument == "--signal-url") options.signaling_url = value;
        else throw std::runtime_error("unknown option: " + std::string(argument));
    }
    if (options.signaling_url.empty()) {
        throw std::runtime_error("--signal-url is required");
    }
    return options;
}

void validate_stream_header(const remoe::protocol::StreamHeader& header) {
    if (header.magic != remoe::protocol::kStreamMagic ||
        header.version != remoe::protocol::kVersion ||
        header.header_size != sizeof(header)) {
        throw std::runtime_error("host sent an incompatible stream header");
    }
    if (header.codec != remoe::protocol::kCodecAv1 || header.codec_profile != 0) {
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

bool send_control_event(remoe::WebRtcTransport& control,
                        const remoe::protocol::InputEvent& event) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&event);
    return control.send_binary(std::span<const std::uint8_t>(bytes, sizeof(event)));
}

bool send_clipboard_text(remoe::WebRtcTransport& control, std::string_view text,
                         std::uint32_t sequence) {
    auto message = remoe::make_clipboard_message(text, sequence);
    return control.send_binary(message);
}

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

class VideoFrameAssembler {
public:
    std::optional<EncodedFrame> consume(std::span<const std::uint8_t> message) {
        if (message.size() <= sizeof(remoe::protocol::VideoChunkHeader)) {
            throw std::runtime_error("host sent a truncated WebRTC video chunk");
        }
        remoe::protocol::VideoChunkHeader header;
        std::memcpy(&header, message.data(), sizeof(header));
        constexpr std::uint32_t max_frame_size = 64 * 1024 * 1024;
        const std::size_t payload_size = message.size() - sizeof(header);
        if (header.magic != remoe::protocol::kVideoChunkMagic ||
            header.version != remoe::protocol::kVersion ||
            header.header_size != sizeof(header) || header.frame_size == 0 ||
            header.frame_size > max_frame_size ||
            header.chunk_offset % remoe::protocol::kVideoChunkPayloadSize != 0 ||
            payload_size > remoe::protocol::kVideoChunkPayloadSize ||
            header.chunk_offset + payload_size > header.frame_size ||
            (header.chunk_offset + payload_size != header.frame_size &&
             payload_size != remoe::protocol::kVideoChunkPayloadSize)) {
            throw std::runtime_error("host sent an invalid WebRTC video chunk");
        }

        const std::size_t chunk_count =
            (header.frame_size + remoe::protocol::kVideoChunkPayloadSize - 1) /
            remoe::protocol::kVideoChunkPayloadSize;
        auto [it, inserted] = frames_.try_emplace(header.frame_number);
        Assembly& assembly = it->second;
        if (inserted) {
            assembly.frame.header.payload_size = header.frame_size;
            assembly.frame.header.flags = header.flags;
            assembly.frame.header.frame_number = header.frame_number;
            assembly.frame.header.timestamp_us = header.timestamp_us;
            assembly.frame.payload.resize(header.frame_size);
            assembly.received.assign(chunk_count, false);
        } else if (assembly.frame.payload.size() != header.frame_size ||
                   assembly.frame.header.flags != header.flags ||
                   assembly.frame.header.timestamp_us != header.timestamp_us) {
            throw std::runtime_error("host sent inconsistent WebRTC video chunks");
        }

        const std::size_t chunk_index =
            header.chunk_offset / remoe::protocol::kVideoChunkPayloadSize;
        if (!assembly.received[chunk_index]) {
            std::memcpy(assembly.frame.payload.data() + header.chunk_offset,
                        message.data() + sizeof(header), payload_size);
            assembly.received[chunk_index] = true;
            ++assembly.received_count;
        }
        if (assembly.received_count != assembly.received.size()) {
            prune(header.frame_number);
            return std::nullopt;
        }

        EncodedFrame completed = std::move(assembly.frame);
        frames_.erase(it);
        prune(header.frame_number);
        return completed;
    }

    bool take_loss_detected() noexcept {
        return std::exchange(loss_detected_, false);
    }

private:
    struct Assembly {
        EncodedFrame frame;
        std::vector<bool> received;
        std::size_t received_count = 0;
    };

    void prune(std::uint64_t newest) {
        constexpr std::uint64_t retained_frames = 8;
        for (auto it = frames_.begin(); it != frames_.end();) {
            if (it->first + retained_frames < newest) {
                loss_detected_ = true;
                it = frames_.erase(it);
            } else ++it;
        }
    }

    std::unordered_map<std::uint64_t, Assembly> frames_;
    bool loss_detected_ = false;
};

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
    remoe::protocol::ClientConfig request;
    request.fps_num = options.fps;
    request.bitrate_bps = options.bitrate_mbps * 1'000'000u;
    request.scale_percent = options.scale_percent;
    request.flags = remoe::protocol::kClientClipboardText;

    struct ClientSessionState {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<remoe::protocol::StreamHeader> stream_header;
        std::string error;
        bool closed = false;
        remoe::VideoWindow* window = nullptr;
        EncodedFrameQueue* queue = nullptr;
        remoe::WebRtcTransport* transport = nullptr;
        VideoFrameAssembler assembler;
        std::chrono::steady_clock::time_point statistics_epoch =
            std::chrono::steady_clock::now();
        std::uint64_t video_bytes = 0;
        std::uint64_t network_bytes = 0;
        std::atomic<DWORD> clipboard_sequence{GetClipboardSequenceNumber()};
        std::atomic_uint32_t outbound_clipboard_sequence{0};
    };
    auto state = std::make_shared<ClientSessionState>();

    const auto fail = [state](std::string error) {
        std::lock_guard lock(state->mutex);
        if (state->error.empty()) state->error = std::move(error);
        if (state->window) state->window->request_close();
        if (state->queue) state->queue->stop();
        state->changed.notify_all();
    };

    remoe::WebRtcTransport::Callbacks control_callbacks;
    control_callbacks.on_local_candidate = [](auto candidate) {
        if (candidate.candidate.find(" typ srflx ") != std::string::npos) {
            std::cout << "WebRTC STUN reflexive candidate gathered\n";
        }
    };
    control_callbacks.on_open = [] {
        std::cout << "WebRTC control DataChannel connected\n";
    };
    control_callbacks.on_video_open = [] {
        std::cout << "WebRTC video DataChannel connected\n";
    };
    control_callbacks.on_binary = [state, fail](std::vector<std::uint8_t> message) {
        if (message.size() >= sizeof(remoe::protocol::ClipboardHeader)) {
            remoe::protocol::ClipboardHeader clipboard;
            std::memcpy(&clipboard, message.data(), sizeof(clipboard));
            if (clipboard.magic == remoe::protocol::kClipboardMagic) {
                if (!remoe::validate_clipboard_message(message)) {
                    fail("host sent an invalid clipboard message");
                    return;
                }
                if (remoe::write_clipboard_text(remoe::clipboard_message_text(message))) {
                    state->clipboard_sequence = GetClipboardSequenceNumber();
                } else {
                    std::cerr << "Could not update the Windows clipboard\n";
                }
                return;
            }
        }
        if (message.size() != sizeof(remoe::protocol::StreamHeader)) {
            fail("host sent an unexpected WebRTC control message");
            return;
        }
        remoe::protocol::StreamHeader header;
        std::memcpy(&header, message.data(), sizeof(header));
        {
            std::lock_guard lock(state->mutex);
            if (state->stream_header) {
                if (state->error.empty()) state->error = "host sent multiple stream headers";
            } else {
                state->stream_header = header;
            }
        }
        state->changed.notify_all();
    };
    control_callbacks.on_video_binary = [state, fail](std::vector<std::uint8_t> message) {
        try {
            std::unique_lock lock(state->mutex);
            if (!state->queue || !state->window || !state->transport) return;
            auto frame = state->assembler.consume(message);
            const bool chunk_loss = state->assembler.take_loss_detected();
            state->network_bytes += message.size();
            bool request_key_frame = chunk_loss;
            if (frame) {
                state->video_bytes += frame->payload.size();
                const auto now = std::chrono::steady_clock::now();
                const double elapsed =
                    std::chrono::duration<double>(now - state->statistics_epoch).count();
                if (elapsed >= 1.0) {
                    state->window->update_transfer_statistics(
                        static_cast<double>(state->video_bytes) * 8.0 / elapsed / 1'000'000.0,
                        static_cast<double>(state->network_bytes) / elapsed / 1'000'000.0);
                    state->statistics_epoch = now;
                    state->video_bytes = 0;
                    state->network_bytes = 0;
                }
                request_key_frame = state->queue->push(std::move(*frame)) ==
                    EncodedFrameQueue::PushResult::RequestKeyFrame || request_key_frame;
            }
            remoe::WebRtcTransport* transport = state->transport;
            lock.unlock();
            if (request_key_frame) {
                remoe::protocol::InputEvent key_frame;
                key_frame.type = remoe::protocol::InputType::RequestKeyFrame;
                if (!send_control_event(*transport, key_frame)) {
                    throw std::runtime_error("failed to request an AV1 key frame");
                }
                std::cerr << "Requested an immediate key frame from the host\n";
            }
        } catch (const std::exception& error) {
            fail(error.what());
        }
    };
    control_callbacks.on_closed = [state] {
        std::lock_guard lock(state->mutex);
        state->closed = true;
        if (state->window) state->window->request_close();
        if (state->queue) state->queue->stop();
        state->changed.notify_all();
    };
    control_callbacks.on_error = [fail](std::string error) {
        std::cerr << "WebRTC error: " << error << '\n';
        fail(std::move(error));
    };

    std::cout << "Connecting through WebRTC invite URL...\n";
    auto control_channel = remoe::establish_webrtc_over_websocket(
        remoe::WebRtcTransport::Role::Offerer, options.signaling_url,
        std::move(control_callbacks));
    {
        std::lock_guard lock(state->mutex);
        state->transport = control_channel.get();
    }
    if (!control_channel->send_binary(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(&request), sizeof(request)))) {
        throw std::runtime_error("failed to send the protocol v7 stream request");
    }

    remoe::protocol::StreamHeader stream_header;
    {
        std::unique_lock lock(state->mutex);
        state->changed.wait(lock, [&] {
            return state->stream_header.has_value() || state->closed || !state->error.empty();
        });
        if (!state->error.empty()) throw std::runtime_error(state->error);
        if (!state->stream_header) {
            throw std::runtime_error("WebRTC connection closed before the stream header");
        }
        stream_header = *state->stream_header;
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
    window.set_input_callback([&control_channel](const remoe::protocol::InputEvent& event) {
        return send_control_event(*control_channel, event);
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
    {
        std::lock_guard lock(state->mutex);
        state->window = &window;
        state->queue = &queue;
        state->statistics_epoch = std::chrono::steady_clock::now();
    }
    remoe::protocol::StreamReady ready;
    if (!control_channel->send_binary(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(&ready), sizeof(ready)))) {
        throw std::runtime_error("failed to acknowledge the WebRTC video stream");
    }
    std::thread decoder(decode_frames, std::ref(window), std::ref(queue),
                        std::ref(pipeline));
    std::atomic_bool clipboard_running{true};
    std::thread clipboard_monitor([&] {
        while (clipboard_running && control_channel->is_open()) {
            const DWORD current = GetClipboardSequenceNumber();
            const DWORD previous = state->clipboard_sequence.exchange(current);
            if (current != previous) {
                if (auto text = remoe::read_clipboard_text(); text &&
                    !send_clipboard_text(*control_channel, *text,
                                         state->outbound_clipboard_sequence++)) {
                    fail("failed to send clipboard text to the host");
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    const int result = window.message_loop();
    window.stop();
    queue.stop();
    clipboard_running = false;
    clipboard_monitor.join();
    control_channel->close();
    decoder.join();
    {
        std::lock_guard lock(state->mutex);
        state->window = nullptr;
        state->queue = nullptr;
        state->transport = nullptr;
    }
    {
        std::lock_guard lock(state->mutex);
        if (!state->error.empty()) throw std::runtime_error(state->error);
    }
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
