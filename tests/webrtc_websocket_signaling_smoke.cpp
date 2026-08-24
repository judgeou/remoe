#include "webrtc_websocket_signaling.h"

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
        std::mutex mutex;
        std::condition_variable received;
        std::vector<std::uint8_t> received_message;

        remoe::WebRtcTransport::Callbacks host_callbacks;
        host_callbacks.on_binary = [&](std::vector<std::uint8_t> message) {
            {
                std::lock_guard lock(mutex);
                received_message = std::move(message);
            }
            received.notify_all();
        };

        auto host_future = std::async(std::launch::async, [&] {
            return remoe::establish_webrtc_over_websocket(
                remoe::WebRtcTransport::Role::Answerer, invite,
                std::move(host_callbacks), 15s);
        });
        auto client_future = std::async(std::launch::async, [&] {
            return remoe::establish_webrtc_over_websocket(
                remoe::WebRtcTransport::Role::Offerer, invite, {}, 15s);
        });

        auto host = host_future.get();
        auto client = client_future.get();
        const std::vector<std::uint8_t> expected{0x52, 0x4d, 0x4f, 0x45};
        if (!client->send_binary(expected)) {
            throw std::runtime_error("WebRTC DataChannel rejected the smoke message");
        }
        {
            std::unique_lock lock(mutex);
            if (!received.wait_for(lock, 5s, [&] { return !received_message.empty(); })) {
                throw std::runtime_error("Timed out receiving the WebRTC smoke message");
            }
        }
        if (received_message != expected) {
            throw std::runtime_error("WebRTC smoke message changed in transit");
        }
        std::cout << "WebSocket signaling smoke test passed via " << url << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebSocket signaling smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
