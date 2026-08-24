#include "protocol.h"
#include "webrtc_tcp_bootstrap.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct BytePipe {
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::uint8_t> bytes;
};

remoe::WebRtcTcpBootstrapIo make_io(BytePipe& inbound, BytePipe& outbound) {
    remoe::WebRtcTcpBootstrapIo io;
    io.send_all = [&outbound](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        {
            std::lock_guard lock(outbound.mutex);
            outbound.bytes.insert(outbound.bytes.end(), bytes, bytes + size);
        }
        outbound.available.notify_all();
        return true;
    };
    io.receive_all = [&inbound](void* data, std::size_t size, auto deadline) {
        auto* output = static_cast<std::uint8_t*>(data);
        std::unique_lock lock(inbound.mutex);
        if (!inbound.available.wait_until(lock, deadline,
                [&] { return inbound.bytes.size() >= size; })) {
            return false;
        }
        for (std::size_t i = 0; i < size; ++i) {
            output[i] = inbound.bytes.front();
            inbound.bytes.pop_front();
        }
        return true;
    };
    return io;
}

} // namespace

int main() {
    try {
        BytePipe client_to_host;
        BytePipe host_to_client;
        std::mutex received_mutex;
        std::condition_variable received_changed;
        remoe::protocol::InputEvent received_event;
        bool received = false;

        remoe::WebRtcTransport::Callbacks host_callbacks;
        host_callbacks.on_binary = [&](std::vector<std::uint8_t> message) {
            if (message.size() != sizeof(received_event)) return;
            {
                std::lock_guard lock(received_mutex);
                std::memcpy(&received_event, message.data(), sizeof(received_event));
                received = true;
            }
            received_changed.notify_all();
        };

        auto host_future = std::async(std::launch::async, [&] {
            return remoe::establish_webrtc_over_tcp(
                remoe::WebRtcTransport::Role::Answerer,
                make_io(client_to_host, host_to_client), std::move(host_callbacks), 10s);
        });
        auto client_future = std::async(std::launch::async, [&] {
            return remoe::establish_webrtc_over_tcp(
                remoe::WebRtcTransport::Role::Offerer,
                make_io(host_to_client, client_to_host), {}, 10s);
        });

        auto host = host_future.get();
        auto client = client_future.get();
        if (!host->is_open() || !client->is_open()) {
            throw std::runtime_error("bootstrap returned before the DataChannel opened");
        }
        {
            std::scoped_lock lock(client_to_host.mutex, host_to_client.mutex);
            if (!client_to_host.bytes.empty() || !host_to_client.bytes.empty()) {
                throw std::runtime_error("bootstrap left signaling bytes in the TCP stream");
            }
        }

        remoe::protocol::InputEvent event;
        event.type = remoe::protocol::InputType::Keyboard;
        event.value1 = 0x1e;
        event.sequence = 42;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&event);
        if (!client->send_binary(std::span<const std::uint8_t>(bytes, sizeof(event)))) {
            throw std::runtime_error("DataChannel rejected the control event");
        }

        {
            std::unique_lock lock(received_mutex);
            if (!received_changed.wait_for(lock, 5s, [&] { return received; })) {
                throw std::runtime_error("host did not receive the control event");
            }
        }
        if (received_event.magic != remoe::protocol::kInputMagic ||
            received_event.version != remoe::protocol::kVersion ||
            received_event.type != event.type || received_event.value1 != event.value1 ||
            received_event.sequence != event.sequence) {
            throw std::runtime_error("control event changed in transit");
        }

        std::cout << "WebRTC TCP bootstrap smoke test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebRTC TCP bootstrap smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
