#include "webrtc_websocket_signaling.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
}

int main(int argc, char** argv) {
    try {
        const std::string url = argc > 1 ? argv[1] : "ws://127.0.0.1:8080/signal";
        const std::string invite = remoe::create_webrtc_signaling_invite(url);
        const std::size_t fragment = invite.find('#');
        constexpr std::string_view nano_id_alphabet =
            "_-0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        if (fragment == std::string::npos || invite.size() - fragment - 1 != 21 ||
            invite.find_first_not_of(nano_id_alphabet, fragment + 1) != std::string::npos) {
            throw std::runtime_error("Generated signaling invite does not contain a valid Nano ID");
        }
        if (argc > 2 && std::string_view(argv[2]) == "--idle-host") {
            std::atomic_bool stop{false};
            auto idle_host = std::async(std::launch::async, [&] {
                return remoe::establish_webrtc_over_websocket(
                    remoe::WebRtcTransport::Role::Answerer, invite, {},
                    (std::chrono::milliseconds::max)(), [&] { return stop.load(); });
            });
            std::this_thread::sleep_for(16s);
            if (idle_host.wait_for(0s) != std::future_status::timeout) {
                (void)idle_host.get();
                throw std::runtime_error("Idle host stopped waiting before a client connected");
            }
            stop = true;
            try {
                (void)idle_host.get();
                throw std::runtime_error("Idle host did not honor the stop request");
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) ==
                    "Idle host did not honor the stop request") throw;
            }
            std::cout << "Idle WebSocket host remained available beyond 15 seconds\n";
            return 0;
        }
        std::mutex mutex;
        std::condition_variable received;
        std::vector<std::uint8_t> received_message;
        std::vector<std::uint8_t> received_video;
        std::atomic_bool host_srflx{false};
        std::atomic_bool client_srflx{false};
        std::atomic_bool host_video_open{false};
        std::atomic_bool client_video_open{false};
        std::atomic_bool host_registered{false};

        remoe::WebRtcTransport::Callbacks host_callbacks;
        host_callbacks.on_local_candidate = [&](auto candidate) {
            if (candidate.candidate.find(" typ srflx") != std::string::npos) {
                host_srflx = true;
            }
        };
        host_callbacks.on_binary = [&](std::vector<std::uint8_t> message) {
            {
                std::lock_guard lock(mutex);
                received_message = std::move(message);
            }
            received.notify_all();
        };
        host_callbacks.on_video_open = [&] {
            host_video_open = true;
            received.notify_all();
        };

        auto host_future = std::async(std::launch::async, [&] {
            return remoe::establish_webrtc_over_websocket(
                remoe::WebRtcTransport::Role::Answerer, invite,
                std::move(host_callbacks), 15s, {}, [&] {
                    host_registered = true;
                    received.notify_all();
                }, remoe::WebRtcTransport::VideoCodec::H264);
        });
        {
            std::unique_lock lock(mutex);
            if (!received.wait_for(lock, 5s, [&] { return host_registered.load(); })) {
                throw std::runtime_error("Timed out registering the host invite");
            }
        }
        remoe::WebRtcTransport::Callbacks client_callbacks;
        client_callbacks.on_local_candidate = [&](auto candidate) {
            if (candidate.candidate.find(" typ srflx") != std::string::npos) {
                client_srflx = true;
            }
        };
        client_callbacks.on_video_open = [&] {
            client_video_open = true;
            received.notify_all();
        };
        client_callbacks.on_video_frame = [&](std::vector<std::uint8_t> frame,
                                               std::uint64_t, bool) {
            {
                std::lock_guard lock(mutex);
                received_video = std::move(frame);
            }
            received.notify_all();
        };
        auto client_future = std::async(std::launch::async, [&] {
            return remoe::establish_webrtc_over_websocket(
                remoe::WebRtcTransport::Role::Offerer, invite,
                std::move(client_callbacks), 15s, {}, {},
                remoe::WebRtcTransport::VideoCodec::H264);
        });

        auto host = host_future.get();
        auto client = client_future.get();
        {
            std::unique_lock lock(mutex);
            if (!received.wait_for(lock, 5s, [&] {
                    return host_video_open.load() && client_video_open.load();
                })) {
                throw std::runtime_error("Timed out opening the WebRTC video track");
            }
        }
        if (!host_srflx.load() || !client_srflx.load()) {
            throw std::runtime_error("STUN did not produce srflx candidates for both peers");
        }
        const std::vector<std::uint8_t> expected{0x52, 0x4d, 0x4f, 0x45};
        if (!client->send_binary(expected)) {
            throw std::runtime_error("WebRTC DataChannel rejected the smoke message");
        }
        const std::vector<std::uint8_t> expected_video{
            0x00, 0x00, 0x00, 0x01, 0x65, 0x56, 0x49, 0x44, 0x45, 0x4f};
        if (!host->send_video_frame(expected_video, 123'456)) {
            throw std::runtime_error("WebRTC video track rejected the smoke frame");
        }
        {
            std::unique_lock lock(mutex);
            if (!received.wait_for(lock, 5s, [&] {
                    return !received_message.empty() && !received_video.empty();
                })) {
                throw std::runtime_error("Timed out receiving the WebRTC smoke message");
            }
        }
        if (received_message != expected) {
            throw std::runtime_error("WebRTC smoke message changed in transit");
        }
        if (received_video != expected_video) {
            throw std::runtime_error("WebRTC video smoke message changed in transit");
        }
        std::cout << "WebSocket signaling smoke test passed via " << url << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebSocket signaling smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
