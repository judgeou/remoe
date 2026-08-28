#include "webrtc_transport.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

enum class EventKind {
    Description,
    Candidate,
};

struct SignalingEvent {
    EventKind kind;
    bool from_offerer;
    std::string value;
    std::string metadata;
};

struct SharedState {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<SignalingEvent> signaling_events;
    bool offerer_open = false;
    bool answerer_open = false;
    bool offerer_video_open = false;
    bool answerer_video_open = false;
    std::string answerer_text;
    std::vector<std::uint8_t> offerer_binary;
    std::vector<std::uint8_t> offerer_video_frame;
    bool answerer_keyframe_requested = false;
    std::vector<std::string> errors;
};

remoe::WebRtcTransport::Callbacks callbacks_for(SharedState& state, bool offerer) {
    remoe::WebRtcTransport::Callbacks callbacks;
    callbacks.on_local_description = [&state, offerer](auto description) {
        {
            std::lock_guard lock(state.mutex);
            state.signaling_events.push_back(
                {EventKind::Description, offerer, std::move(description.sdp),
                    std::move(description.type)});
        }
        state.changed.notify_all();
    };
    callbacks.on_local_candidate = [&state, offerer](auto candidate) {
        {
            std::lock_guard lock(state.mutex);
            state.signaling_events.push_back(
                {EventKind::Candidate, offerer, std::move(candidate.candidate),
                    std::move(candidate.mid)});
        }
        state.changed.notify_all();
    };
    callbacks.on_open = [&state, offerer] {
        {
            std::lock_guard lock(state.mutex);
            (offerer ? state.offerer_open : state.answerer_open) = true;
        }
        state.changed.notify_all();
    };
    callbacks.on_video_open = [&state, offerer] {
        {
            std::lock_guard lock(state.mutex);
            (offerer ? state.offerer_video_open : state.answerer_video_open) = true;
        }
        state.changed.notify_all();
    };
    callbacks.on_text = [&state, offerer](std::string message) {
        if (offerer) return;
        {
            std::lock_guard lock(state.mutex);
            state.answerer_text = std::move(message);
        }
        state.changed.notify_all();
    };
    callbacks.on_binary = [&state, offerer](std::vector<std::uint8_t> message) {
        if (!offerer) return;
        {
            std::lock_guard lock(state.mutex);
            state.offerer_binary = std::move(message);
        }
        state.changed.notify_all();
    };
    callbacks.on_video_frame = [&state, offerer](std::vector<std::uint8_t> message,
                                                  std::uint64_t, bool) {
        if (!offerer) return;
        {
            std::lock_guard lock(state.mutex);
            state.offerer_video_frame = std::move(message);
        }
        state.changed.notify_all();
    };
    callbacks.on_video_keyframe_requested = [&state, offerer] {
        if (offerer) return;
        {
            std::lock_guard lock(state.mutex);
            state.answerer_keyframe_requested = true;
        }
        state.changed.notify_all();
    };
    callbacks.on_error = [&state, offerer](std::string error) {
        {
            std::lock_guard lock(state.mutex);
            state.errors.push_back(std::string(offerer ? "offerer: " : "answerer: ") + error);
        }
        state.changed.notify_all();
    };
    return callbacks;
}

bool pump_until(SharedState& state, remoe::WebRtcTransport& offerer,
    remoe::WebRtcTransport& answerer, std::chrono::steady_clock::time_point deadline,
    const auto& complete) {
    while (std::chrono::steady_clock::now() < deadline) {
        SignalingEvent event{};
        bool has_event = false;
        {
            std::unique_lock lock(state.mutex);
            if (complete()) return true;
            state.changed.wait_for(lock, 50ms,
                [&] { return !state.signaling_events.empty() || complete(); });
            if (complete()) return true;
            if (!state.signaling_events.empty()) {
                event = std::move(state.signaling_events.front());
                state.signaling_events.pop_front();
                has_event = true;
            }
        }

        if (!has_event) continue;
        auto& destination = event.from_offerer ? answerer : offerer;
        if (event.kind == EventKind::Description) {
            destination.set_remote_description(event.value, event.metadata);
        } else {
            destination.add_remote_candidate(event.value, event.metadata);
        }
    }
    return false;
}

} // namespace

int main() {
    try {
        bool turn_rejected = false;
        try {
            remoe::WebRtcTransport::Configuration forbidden_config;
            forbidden_config.ice_servers = {"turn:example.invalid:3478"};
            remoe::WebRtcTransport forbidden_transport(forbidden_config, {});
        } catch (const std::invalid_argument&) {
            turn_rejected = true;
        }
        if (!turn_rejected) {
            throw std::runtime_error("WebRTC transport accepted a TURN server");
        }

        SharedState state;

        remoe::WebRtcTransport::Configuration answerer_config;
        answerer_config.role = remoe::WebRtcTransport::Role::Answerer;
        answerer_config.video_direction = remoe::WebRtcTransport::VideoDirection::SendOnly;
        answerer_config.video_codec = remoe::WebRtcTransport::VideoCodec::H264;
        remoe::WebRtcTransport answerer(answerer_config, callbacks_for(state, false));

        remoe::WebRtcTransport::Configuration offerer_config;
        offerer_config.role = remoe::WebRtcTransport::Role::Offerer;
        offerer_config.video_direction = remoe::WebRtcTransport::VideoDirection::ReceiveOnly;
        offerer_config.video_codec = remoe::WebRtcTransport::VideoCodec::H264;
        remoe::WebRtcTransport offerer(offerer_config, callbacks_for(state, true));

        answerer.start();
        offerer.start();

        const auto open_deadline = std::chrono::steady_clock::now() + 15s;
        const bool connected = pump_until(state, offerer, answerer, open_deadline, [&] {
            return state.offerer_open && state.answerer_open &&
                state.offerer_video_open && state.answerer_video_open;
        });
        if (!connected) {
            std::cerr << "Timed out opening the host-candidate DataChannel\n";
            return 1;
        }

        constexpr std::string_view text = "remoe-webrtc-smoke";
        const std::array<std::uint8_t, 4> binary = {0x52, 0x4d, 0x4f, 0x45};
        const std::vector<std::uint8_t> video = {
            0x00, 0x00, 0x00, 0x01, 0x65, 0x52, 0x4d, 0x4f, 0x45};
        if (!offerer.send_text(text) || !answerer.send_binary(binary) ||
            !answerer.send_video_frame(video, 123'456)) {
            std::cerr << "WebRTC transport rejected an outbound message\n";
            return 1;
        }

        const auto message_deadline = std::chrono::steady_clock::now() + 5s;
        const bool delivered = pump_until(state, offerer, answerer, message_deadline, [&] {
            return state.answerer_text == text && state.offerer_binary ==
                std::vector<std::uint8_t>(binary.begin(), binary.end()) &&
                state.offerer_video_frame == video;
        });
        if (!delivered) {
            std::cerr << "Timed out delivering DataChannel messages\n";
            return 1;
        }

        if (!offerer.request_video_keyframe()) {
            std::cerr << "Receive track rejected an RTCP PLI request\n";
            return 1;
        }
        const bool pli_delivered = pump_until(state, offerer, answerer,
            std::chrono::steady_clock::now() + 5s, [&] {
                return state.answerer_keyframe_requested;
            });
        if (!pli_delivered) {
            std::cerr << "Timed out delivering RTCP PLI feedback\n";
            return 1;
        }

        {
            std::lock_guard lock(state.mutex);
            if (!state.errors.empty()) {
                for (const auto& error : state.errors) std::cerr << error << '\n';
                return 1;
            }
        }

        const auto stats = offerer.statistics();
        if (!stats.local_candidate || !stats.remote_candidate) {
            std::cerr << "Connected DataChannel has no selected ICE candidate pair\n";
            return 1;
        }

        // Exercise Remoe's RFC 9364 depacketizer too. libdatachannel provides
        // AV1 packetization but does not ship an AV1 depacketizer in 0.24.x.
        SharedState av1_state;
        remoe::WebRtcTransport::Configuration av1_answerer_config;
        av1_answerer_config.role = remoe::WebRtcTransport::Role::Answerer;
        av1_answerer_config.video_direction =
            remoe::WebRtcTransport::VideoDirection::SendOnly;
        av1_answerer_config.video_codec = remoe::WebRtcTransport::VideoCodec::AV1;
        remoe::WebRtcTransport av1_answerer(
            av1_answerer_config, callbacks_for(av1_state, false));
        remoe::WebRtcTransport::Configuration av1_offerer_config;
        av1_offerer_config.role = remoe::WebRtcTransport::Role::Offerer;
        av1_offerer_config.video_direction =
            remoe::WebRtcTransport::VideoDirection::ReceiveOnly;
        av1_offerer_config.video_codec = remoe::WebRtcTransport::VideoCodec::AV1;
        remoe::WebRtcTransport av1_offerer(
            av1_offerer_config, callbacks_for(av1_state, true));
        av1_answerer.start();
        av1_offerer.start();
        if (!pump_until(av1_state, av1_offerer, av1_answerer,
                std::chrono::steady_clock::now() + 15s, [&] {
                    return av1_state.offerer_open && av1_state.answerer_open &&
                        av1_state.offerer_video_open && av1_state.answerer_video_open;
                })) {
            std::cerr << "Timed out opening the AV1 video track\n";
            return 1;
        }
        // A Temporal Delimiter, Sequence Header and a fragmented Frame OBU.
        // RFC 9364 strips the delimiter on send; Remoe must restore it for the
        // native oneVPL low-overhead AV1 decoder. The large OBU also exercises
        // Z/Y continuation across multiple RTP packets.
        std::vector<std::uint8_t> av1_frame = {
            0x12, 0x00, 0x0a, 0x01, 0xaa, 0x32, 0x80, 0x20};
        for (std::size_t index = 0; index < 4096; ++index) {
            av1_frame.push_back(static_cast<std::uint8_t>(index));
        }
        if (!av1_answerer.send_video_frame(av1_frame, 654'321) ||
            !pump_until(av1_state, av1_offerer, av1_answerer,
                std::chrono::steady_clock::now() + 5s, [&] {
                    return av1_state.offerer_video_frame == av1_frame;
                })) {
            std::cerr << "AV1 frame changed during RTP packetization/depacketization\n";
            return 1;
        }

        std::cout << "WebRTC transport smoke test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WebRTC transport smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
