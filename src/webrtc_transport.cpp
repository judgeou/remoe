#include "webrtc_transport.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace remoe {
namespace {

WebRtcTransport::State to_public_state(rtc::PeerConnection::State state) noexcept {
    switch (state) {
    case rtc::PeerConnection::State::New: return WebRtcTransport::State::New;
    case rtc::PeerConnection::State::Connecting: return WebRtcTransport::State::Connecting;
    case rtc::PeerConnection::State::Connected: return WebRtcTransport::State::Connected;
    case rtc::PeerConnection::State::Disconnected: return WebRtcTransport::State::Disconnected;
    case rtc::PeerConnection::State::Failed: return WebRtcTransport::State::Failed;
    case rtc::PeerConnection::State::Closed: return WebRtcTransport::State::Closed;
    }
    return WebRtcTransport::State::Failed;
}

WebRtcTransport::IceState to_public_ice_state(rtc::PeerConnection::IceState state) noexcept {
    switch (state) {
    case rtc::PeerConnection::IceState::New: return WebRtcTransport::IceState::New;
    case rtc::PeerConnection::IceState::Checking: return WebRtcTransport::IceState::Checking;
    case rtc::PeerConnection::IceState::Connected: return WebRtcTransport::IceState::Connected;
    case rtc::PeerConnection::IceState::Completed: return WebRtcTransport::IceState::Completed;
    case rtc::PeerConnection::IceState::Failed: return WebRtcTransport::IceState::Failed;
    case rtc::PeerConnection::IceState::Disconnected: return WebRtcTransport::IceState::Disconnected;
    case rtc::PeerConnection::IceState::Closed: return WebRtcTransport::IceState::Closed;
    }
    return WebRtcTransport::IceState::Failed;
}

WebRtcTransport::GatheringState to_public_gathering_state(
    rtc::PeerConnection::GatheringState state) noexcept {
    switch (state) {
    case rtc::PeerConnection::GatheringState::New: return WebRtcTransport::GatheringState::New;
    case rtc::PeerConnection::GatheringState::InProgress:
        return WebRtcTransport::GatheringState::InProgress;
    case rtc::PeerConnection::GatheringState::Complete:
        return WebRtcTransport::GatheringState::Complete;
    }
    return WebRtcTransport::GatheringState::New;
}

WebRtcTransport::IceCandidate to_public_candidate(const rtc::Candidate& candidate) {
    return {std::string(candidate), candidate.mid()};
}

std::string upper_ascii(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string_view codec_name(WebRtcTransport::VideoCodec codec) noexcept {
    switch (codec) {
    case WebRtcTransport::VideoCodec::H264: return "H264";
    case WebRtcTransport::VideoCodec::AV1: return "AV1";
    }
    return "";
}

std::optional<std::uint8_t> find_payload_type(
    const rtc::Description::Media& media, WebRtcTransport::VideoCodec codec) {
    const std::string expected(codec_name(codec));
    for (const int payload_type : media.payloadTypes()) {
        const auto* map = media.rtpMap(payload_type);
        if (map && upper_ascii(map->format) == expected && payload_type >= 0 &&
            payload_type <= (std::numeric_limits<std::uint8_t>::max)()) {
            return static_cast<std::uint8_t>(payload_type);
        }
    }
    return std::nullopt;
}

std::uint32_t random_ssrc() {
    std::random_device random;
    std::uint32_t value = 0;
    while (value == 0) {
        value = (static_cast<std::uint32_t>(random()) << 16) ^
                static_cast<std::uint32_t>(random());
    }
    return value;
}

bool h264_is_key_frame(std::span<const std::uint8_t> frame) noexcept {
    for (std::size_t index = 0; index + 4 < frame.size(); ++index) {
        std::size_t header = std::string_view::npos;
        if (frame[index] == 0 && frame[index + 1] == 0 && frame[index + 2] == 1) {
            header = index + 3;
        } else if (index + 4 < frame.size() && frame[index] == 0 &&
                   frame[index + 1] == 0 && frame[index + 2] == 0 &&
                   frame[index + 3] == 1) {
            header = index + 4;
        }
        if (header != std::string_view::npos && header < frame.size() &&
            (frame[header] & 0x1fu) == 5) return true;
    }
    return false;
}

bool av1_has_sequence_header(std::span<const std::uint8_t> frame) noexcept {
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const std::uint8_t header = frame[offset++];
        if ((header & 0x81u) != 0) return false;
        const std::uint8_t type = (header >> 3) & 0x0fu;
        if ((header & 0x04u) != 0 && offset++ >= frame.size()) return false;
        if ((header & 0x02u) == 0) return false;
        std::size_t payload_size = 0;
        unsigned shift = 0;
        bool complete = false;
        for (unsigned index = 0; index < 8 && offset < frame.size(); ++index) {
            const std::uint8_t current = frame[offset++];
            if (shift < sizeof(std::size_t) * 8) {
                payload_size |= static_cast<std::size_t>(current & 0x7fu) << shift;
            }
            if ((current & 0x80u) == 0) {
                complete = true;
                break;
            }
            shift += 7;
        }
        if (!complete || payload_size > frame.size() - offset) return false;
        if (type == 1) return true;
        offset += payload_size;
    }
    return false;
}

bool encoded_key_frame(WebRtcTransport::VideoCodec codec,
                       std::span<const std::uint8_t> frame) noexcept {
    return codec == WebRtcTransport::VideoCodec::H264
        ? h264_is_key_frame(frame) : av1_has_sequence_header(frame);
}

// libdatachannel 0.24 has an AV1 RTP packetizer but no matching depacketizer.
// This implements RFC 9364 aggregation/fragment reassembly and restores the
// OBU size fields expected by the existing oneVPL decoder.
class Av1RtpDepacketizer final : public rtc::VideoRtpDepacketizer {
public:
    Av1RtpDepacketizer(std::function<void()> request_keyframe,
                       std::function<void(std::string)> diagnostic)
        : request_keyframe_(std::move(request_keyframe)),
          diagnostic_(std::move(diagnostic)) {}

private:
    struct BufferedFrame {
        message_buffer packets;
        std::optional<std::uint16_t> first_sequence;
        std::optional<std::uint16_t> last_sequence;
        std::chrono::steady_clock::time_point first_seen;
        std::chrono::steady_clock::time_point last_nack{};
        std::uint32_t ssrc = 0;
        bool key_frame = false;
    };

    static constexpr std::size_t kMaxBufferedFrames = 12;
    static constexpr auto kMaxReorderDelay = std::chrono::milliseconds(300);
    static constexpr auto kNackInterval = std::chrono::milliseconds(30);
    static constexpr auto kPliInterval = std::chrono::milliseconds(500);
    static constexpr auto kLossDiagnosticInterval = std::chrono::seconds(1);
    static constexpr std::size_t kMaxNackSequenceSpan = 8192;

    static bool read_leb128(std::span<const rtc::byte> bytes, std::size_t& offset,
                            std::size_t& value) {
        value = 0;
        for (unsigned index = 0; index < 8 && offset < bytes.size(); ++index) {
            const auto current = std::to_integer<std::uint8_t>(bytes[offset++]);
            if (index >= sizeof(std::size_t) && (current & 0x7fu) != 0) return false;
            if (index * 7 < sizeof(std::size_t) * 8) {
                value |= static_cast<std::size_t>(current & 0x7fu) << (index * 7);
            }
            if ((current & 0x80u) == 0) return true;
        }
        return false;
    }

    static void append_leb128(rtc::binary& output, std::size_t value) {
        do {
            std::uint8_t current = static_cast<std::uint8_t>(value & 0x7fu);
            value >>= 7;
            if (value != 0) current |= 0x80u;
            output.push_back(static_cast<rtc::byte>(current));
        } while (value != 0);
    }

    static bool append_obu_with_size(rtc::binary& frame, const rtc::binary& obu) {
        if (obu.empty()) return false;
        const auto header = std::to_integer<std::uint8_t>(obu.front());
        const std::size_t header_size = (header & 0x04u) != 0 ? 2 : 1;
        if (obu.size() < header_size) return false;
        frame.push_back(static_cast<rtc::byte>(header | 0x02u));
        if (header_size == 2) frame.push_back(obu[1]);
        append_leb128(frame, obu.size() - header_size);
        frame.insert(frame.end(), obu.begin() + static_cast<std::ptrdiff_t>(header_size),
                     obu.end());
        return true;
    }

    static bool packet_payload(const rtc::message_ptr& packet,
                               const rtc::RtpHeader*& rtp,
                               std::span<const rtc::byte>& payload) {
        if (!packet || packet->size() < sizeof(rtc::RtpHeader)) return false;
        rtp = reinterpret_cast<const rtc::RtpHeader*>(packet->data());
        const std::size_t header_size = rtp->getSize() + rtp->getExtensionHeaderSize();
        const std::size_t padding_size = rtp->padding()
            ? std::to_integer<std::uint8_t>(packet->back()) : 0;
        if (packet->size() <= header_size + padding_size) return false;
        payload = std::span<const rtc::byte>(
            packet->data() + header_size,
            packet->size() - header_size - padding_size);
        return !payload.empty();
    }

    static bool complete(const BufferedFrame& frame) {
        if (!frame.first_sequence || !frame.last_sequence) return false;
        const std::size_t expected =
            static_cast<std::uint16_t>(*frame.last_sequence - *frame.first_sequence) + 1u;
        if (frame.packets.size() != expected) return false;
        std::uint16_t sequence = *frame.first_sequence;
        for (const auto& packet : frame.packets) {
            const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(packet->data());
            if (rtp->seqNumber() != sequence++) return false;
        }
        return true;
    }

    void request_recovery() {
        waiting_for_key_frame_ = true;
        const auto now = std::chrono::steady_clock::now();
        if (last_pli_ != std::chrono::steady_clock::time_point{} &&
            now - last_pli_ < kPliInterval) return;
        last_pli_ = now;
        diagnose("AV1 recovery requested: sending RTCP PLI");
        if (request_keyframe_) request_keyframe_();
    }

    void diagnose(std::string message) const noexcept {
        if (!diagnostic_) return;
        try {
            diagnostic_(std::move(message));
        } catch (...) {
        }
    }

    void diagnose_packet_loss(std::string message) {
        const auto now = std::chrono::steady_clock::now();
        if (last_loss_diagnostic_ != std::chrono::steady_clock::time_point{} &&
            now - last_loss_diagnostic_ < kLossDiagnosticInterval) {
            ++suppressed_loss_diagnostics_;
            return;
        }
        if (suppressed_loss_diagnostics_ != 0) {
            message += "; suppressed_events=" +
                std::to_string(suppressed_loss_diagnostics_);
            suppressed_loss_diagnostics_ = 0;
        }
        last_loss_diagnostic_ = now;
        diagnose(std::move(message));
    }

    void remember_retired(std::uint32_t timestamp) {
        retired_timestamps_.insert(timestamp);
        retired_order_.push_back(timestamp);
        constexpr std::size_t retained_timestamps = 64;
        if (retired_order_.size() > retained_timestamps) {
            retired_timestamps_.erase(retired_order_.front());
            retired_order_.pop_front();
        }
    }

    void retire_front(bool lost) {
        if (frame_order_.empty()) return;
        const std::uint32_t timestamp = frame_order_.front();
        frame_order_.pop_front();
        if (const auto found = frames_.find(timestamp); found != frames_.end()) {
            if (lost) {
                const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - found->second.first_seen).count();
                diagnose_packet_loss("Dropping incomplete AV1 RTP frame: timestamp=" +
                    std::to_string(timestamp) + ", packets=" +
                    std::to_string(found->second.packets.size()) + ", age_ms=" +
                    std::to_string(age_ms) + ", first=" +
                    (found->second.first_sequence
                        ? std::to_string(*found->second.first_sequence) : "unknown") +
                    ", last=" + (found->second.last_sequence
                        ? std::to_string(*found->second.last_sequence) : "unknown"));
            }
            if (found->second.last_sequence) {
                next_frame_sequence_ = static_cast<std::uint16_t>(
                    *found->second.last_sequence + 1u);
            }
            frames_.erase(found);
        }
        remember_retired(timestamp);
        if (lost) request_recovery();
    }

    void send_missing_nacks(BufferedFrame& frame,
                            const rtc::message_callback& send) {
        if (!send || !frame.first_sequence || !frame.last_sequence) return;
        const auto now = std::chrono::steady_clock::now();
        if (frame.last_nack != std::chrono::steady_clock::time_point{} &&
            now - frame.last_nack < kNackInterval) return;

        std::unordered_set<std::uint16_t> received;
        received.reserve(frame.packets.size());
        for (const auto& packet : frame.packets) {
            const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(packet->data());
            received.insert(rtp->seqNumber());
        }
        std::vector<std::uint16_t> missing;
        std::uint16_t sequence = *frame.first_sequence;
        const std::size_t expected =
            static_cast<std::uint16_t>(*frame.last_sequence - sequence) + 1u;
        if (expected > kMaxNackSequenceSpan) {
            diagnose_packet_loss("Ignoring implausible AV1 RTP sequence span: packets=" +
                std::to_string(frame.packets.size()) + ", span=" +
                std::to_string(expected));
            request_recovery();
            return;
        }
        missing.reserve(expected - (std::min)(expected, received.size()));
        for (std::size_t index = 0; index < expected; ++index, ++sequence) {
            if (!received.contains(sequence)) missing.push_back(sequence);
        }
        if (missing.empty()) return;
        frame.last_nack = now;
        diagnose_packet_loss("AV1 RTP gap: timestamp=" + std::to_string(
            reinterpret_cast<const rtc::RtpHeader*>((*frame.packets.begin())->data())->timestamp()) +
            ", received=" + std::to_string(frame.packets.size()) +
            ", missing=" + std::to_string(missing.size()) + ", sending NACK");

        constexpr std::size_t max_nacks_per_packet = 64;
        for (std::size_t offset = 0; offset < missing.size();
             offset += max_nacks_per_packet) {
            const auto count = static_cast<unsigned int>((std::min)(
                max_nacks_per_packet, missing.size() - offset));
            auto message = rtc::make_message(rtc::RtcpNack::Size(count),
                                             rtc::Message::Control);
            auto* nack = reinterpret_cast<rtc::RtcpNack*>(message->data());
            nack->preparePacket(frame.ssrc, count);
            for (unsigned int index = 0; index < count; ++index) {
                nack->parts[index].setPid(missing[offset + index]);
                nack->parts[index].setBlp(0);
            }
            send(std::move(message));
        }
    }

    void drain_ready(rtc::message_vector& output,
                     const rtc::message_callback& send) {
        const auto now = std::chrono::steady_clock::now();
        while (!frame_order_.empty()) {
            auto found = frames_.find(frame_order_.front());
            if (found == frames_.end()) {
                frame_order_.pop_front();
                continue;
            }
            BufferedFrame& frame = found->second;
            if (!complete(frame)) {
                send_missing_nacks(frame, send);
                if (frames_.size() <= kMaxBufferedFrames &&
                    now - frame.first_seen <= kMaxReorderDelay) break;
                retire_front(true);
                continue;
            }

            if (waiting_for_key_frame_ && !frame.key_frame) {
                ++suppressed_frames_;
                retire_front(false);
                continue;
            }
            auto message = reassemble(frame.packets);
            if (!message) {
                retire_front(true);
                continue;
            }
            if (frame.key_frame) {
                std::size_t packet_bytes = 0;
                for (const auto& packet : frame.packets) packet_bytes += packet->size();
                diagnose("Complete AV1 key frame: packets=" +
                    std::to_string(frame.packets.size()) + ", rtp_bytes=" +
                    std::to_string(packet_bytes) + ", suppressed_delta_frames=" +
                    std::to_string(suppressed_frames_));
                suppressed_frames_ = 0;
                waiting_for_key_frame_ = false;
            }
            output.push_back(std::move(message));
            retire_front(false);
        }
    }

    void incoming(rtc::message_vector& messages,
                  const rtc::message_callback& send) override {
        // libdatachannel may dispatch packets belonging to one track from
        // several receive threads. This handler owns mutable frame assembly
        // state, so preserve packet callback order and prevent container races.
        std::lock_guard incoming_lock(incoming_mutex_);
        rtc::message_vector output;
        for (auto& message : messages) {
            if (message->type == rtc::Message::Control) {
                output.push_back(std::move(message));
                continue;
            }

            const rtc::RtpHeader* rtp = nullptr;
            std::span<const rtc::byte> payload;
            if (!packet_payload(message, rtp, payload)) {
                request_recovery();
                continue;
            }
            const std::uint32_t timestamp = rtp->timestamp();
            if (retired_timestamps_.contains(timestamp)) continue;

            auto [found, inserted] = frames_.try_emplace(timestamp);
            BufferedFrame& frame = found->second;
            if (inserted) {
                frame.first_seen = std::chrono::steady_clock::now();
                frame.ssrc = rtp->ssrc();
                frame.first_sequence = next_frame_sequence_;
                frame_order_.push_back(timestamp);
            }
            const std::uint8_t aggregation = std::to_integer<std::uint8_t>(payload.front());
            const bool starts_obu = (aggregation & 0x80u) == 0;
            const bool starts_sequence = (aggregation & 0x08u) != 0;
            // Z=0 means "not a continuation of the previous OBU", not
            // "first packet of the temporal unit". A temporal unit may contain
            // several OBUs, so never overwrite an established frame boundary.
            if (starts_sequence || (!frame.first_sequence && starts_obu)) {
                frame.first_sequence = rtp->seqNumber();
            }
            if (starts_sequence) frame.key_frame = true;
            if (rtp->marker()) frame.last_sequence = rtp->seqNumber();
            frame.packets.insert(std::move(message));
        }
        drain_ready(output, send);
        messages.swap(output);
    }

    rtc::message_ptr reassemble(message_buffer& buffer) override {
        if (buffer.empty()) return nullptr;
        const auto first = *buffer.begin();
        const auto* first_header = reinterpret_cast<const rtc::RtpHeader*>(first->data());
        const std::uint8_t payload_type = first_header->payloadType();
        const std::uint32_t timestamp = first_header->timestamp();
        std::uint16_t expected_sequence = first_header->seqNumber();
        // RFC 9364 requires Temporal Delimiter OBUs to be removed before RTP
        // packetization. Restore one at the start of every reassembled temporal
        // unit: the native oneVPL decoder consumes low-overhead AV1 bitstreams
        // and relies on this boundary when complete frames are submitted.
        rtc::binary frame = {
            static_cast<rtc::byte>(0x12),
            static_cast<rtc::byte>(0x00),
        };
        rtc::binary partial_obu;
        bool has_partial_obu = false;

        for (const auto& packet : buffer) {
            const auto* rtp = reinterpret_cast<const rtc::RtpHeader*>(packet->data());
            if (rtp->seqNumber() != expected_sequence++) return nullptr;
            const std::size_t header_size = rtp->getSize() + rtp->getExtensionHeaderSize();
            const std::size_t padding_size = rtp->padding()
                ? std::to_integer<std::uint8_t>(packet->back()) : 0;
            if (packet->size() <= header_size + padding_size) return nullptr;

            const std::span<const rtc::byte> payload(
                packet->data() + header_size,
                packet->size() - header_size - padding_size);
            const std::uint8_t aggregation = std::to_integer<std::uint8_t>(payload[0]);
            const bool continues_previous = (aggregation & 0x80u) != 0;
            const bool continues_next = (aggregation & 0x40u) != 0;
            const unsigned obu_count = (aggregation >> 4) & 0x03u;
            std::size_t offset = 1;
            std::vector<std::span<const rtc::byte>> elements;

            if (obu_count == 0) {
                while (offset < payload.size()) {
                    std::size_t length = 0;
                    if (!read_leb128(payload, offset, length) ||
                        length > payload.size() - offset) return nullptr;
                    elements.emplace_back(payload.data() + offset, length);
                    offset += length;
                }
            } else {
                elements.reserve(obu_count);
                for (unsigned index = 0; index < obu_count; ++index) {
                    std::size_t length = payload.size() - offset;
                    if (index + 1 < obu_count &&
                        (!read_leb128(payload, offset, length) ||
                         length > payload.size() - offset)) return nullptr;
                    elements.emplace_back(payload.data() + offset, length);
                    offset += length;
                }
                if (offset != payload.size()) return nullptr;
            }
            if (elements.empty()) return nullptr;

            for (std::size_t index = 0; index < elements.size(); ++index) {
                const bool continuation = index == 0 && continues_previous;
                const bool fragmented = index + 1 == elements.size() && continues_next;
                const auto element = elements[index];
                if (continuation) {
                    if (!has_partial_obu) return nullptr;
                    partial_obu.insert(partial_obu.end(), element.begin(), element.end());
                } else {
                    if (has_partial_obu) return nullptr;
                    partial_obu.assign(element.begin(), element.end());
                }
                has_partial_obu = fragmented;
                if (!fragmented) {
                    if (!append_obu_with_size(frame, partial_obu)) return nullptr;
                    partial_obu.clear();
                }
            }
        }
        if (has_partial_obu || frame.size() == 2) return nullptr;
        return rtc::make_message(std::move(frame), createFrameInfo(timestamp, payload_type));
    }

    std::function<void()> request_keyframe_;
    std::function<void(std::string)> diagnostic_;
    std::mutex incoming_mutex_;
    std::unordered_map<std::uint32_t, BufferedFrame> frames_;
    std::deque<std::uint32_t> frame_order_;
    std::unordered_set<std::uint32_t> retired_timestamps_;
    std::deque<std::uint32_t> retired_order_;
    std::optional<std::uint16_t> next_frame_sequence_;
    std::chrono::steady_clock::time_point last_pli_{};
    std::chrono::steady_clock::time_point last_loss_diagnostic_{};
    bool waiting_for_key_frame_ = false;
    std::size_t suppressed_frames_ = 0;
    std::size_t suppressed_loss_diagnostics_ = 0;
};

template <typename Callback, typename... Args>
void invoke_callback(const Callback& callback, Args&&... args) noexcept {
    if (!callback) return;
    try {
        callback(std::forward<Args>(args)...);
    } catch (...) {
        // Never allow application exceptions to cross a libdatachannel thread.
    }
}

} // namespace

struct WebRtcTransport::Impl : std::enable_shared_from_this<WebRtcTransport::Impl> {
    Configuration configuration;
    Callbacks callbacks;
    std::shared_ptr<rtc::PeerConnection> peer_connection;
    std::shared_ptr<rtc::DataChannel> data_channel;
    std::shared_ptr<rtc::Track> video_track;
    mutable std::mutex channel_mutex;
    std::mutex video_timestamp_mutex;
    std::uint32_t last_video_timestamp = 0;
    std::uint64_t video_timestamp_wraps = 0;
    bool have_video_timestamp = false;
    std::atomic<State> connection_state{State::New};
    std::atomic<IceState> current_ice_state{IceState::New};
    std::atomic<GatheringState> current_gathering_state{GatheringState::New};
    std::atomic_bool started{false};
    std::atomic_bool open{false};
    std::atomic_bool video_open{false};
    std::atomic_bool closing{false};
    std::atomic_bool closed_notified{false};

    Impl(Configuration config, Callbacks callback_set)
        : configuration(std::move(config)), callbacks(std::move(callback_set)) {}

    void initialize() {
        if (configuration.data_channel_label.empty()) {
            throw std::invalid_argument("WebRTC data channel label must not be empty");
        }
        if (configuration.video_direction == VideoDirection::SendOnly &&
            configuration.role != Role::Answerer) {
            throw std::invalid_argument("WebRTC sending video track must use the answerer role");
        }
        if (configuration.video_direction == VideoDirection::ReceiveOnly &&
            configuration.role != Role::Offerer) {
            throw std::invalid_argument("WebRTC receiving video track must use the offerer role");
        }
        if (configuration.port_range_begin > configuration.port_range_end) {
            throw std::invalid_argument("WebRTC ICE port range is invalid");
        }

        static std::once_flag logger_once;
        std::call_once(logger_once, [] { rtc::InitLogger(rtc::LogLevel::Warning); });

        rtc::Configuration rtc_config;
        for (const auto& url : configuration.ice_servers) {
            rtc::IceServer server(url);
            if (server.type != rtc::IceServer::Type::Stun || !url.starts_with("stun:")) {
                throw std::invalid_argument("WebRTC transport accepts STUN servers only");
            }
            rtc_config.iceServers.push_back(std::move(server));
        }
        rtc_config.bindAddress = configuration.bind_address;
        rtc_config.portRangeBegin = configuration.port_range_begin;
        rtc_config.portRangeEnd = configuration.port_range_end;
        rtc_config.enableIceTcp = configuration.enable_ice_tcp;

        peer_connection = std::make_shared<rtc::PeerConnection>(std::move(rtc_config));
        const std::weak_ptr<Impl> weak_self = weak_from_this();

        peer_connection->onLocalDescription([weak_self](rtc::Description description) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                invoke_callback(self->callbacks.on_local_description,
                    LocalDescription{std::string(description), description.typeString()});
            }
        });
        peer_connection->onLocalCandidate([weak_self](rtc::Candidate candidate) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                invoke_callback(self->callbacks.on_local_candidate, to_public_candidate(candidate));
            }
        });
        peer_connection->onStateChange([weak_self](rtc::PeerConnection::State state) {
            if (const auto self = weak_self.lock()) {
                const State public_state = to_public_state(state);
                self->connection_state.store(public_state);
                invoke_callback(self->callbacks.on_state_changed, public_state);
            }
        });
        peer_connection->onIceStateChange([weak_self](rtc::PeerConnection::IceState state) {
            if (const auto self = weak_self.lock()) {
                const IceState public_state = to_public_ice_state(state);
                self->current_ice_state.store(public_state);
                invoke_callback(self->callbacks.on_ice_state_changed, public_state);
            }
        });
        peer_connection->onGatheringStateChange(
            [weak_self](rtc::PeerConnection::GatheringState state) {
                if (const auto self = weak_self.lock()) {
                    const GatheringState public_state = to_public_gathering_state(state);
                    self->current_gathering_state.store(public_state);
                    invoke_callback(self->callbacks.on_gathering_state_changed, public_state);
                }
            });
        peer_connection->onDataChannel([weak_self](std::shared_ptr<rtc::DataChannel> channel) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                if (self->configuration.role != Role::Answerer) {
                    channel->close();
                    self->report_error("Unexpected remote-created WebRTC data channel");
                    return;
                }
                if (channel->label() == self->configuration.data_channel_label) {
                    self->attach_data_channel(std::move(channel));
                    return;
                }
                {
                    const std::string label = channel->label();
                    channel->close();
                    self->report_error("Unexpected WebRTC data channel label: " + label);
                }
            }
        });
        peer_connection->onTrack([weak_self](std::shared_ptr<rtc::Track> track) {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                if (self->configuration.video_direction != VideoDirection::SendOnly ||
                    track->description().type() != "video") {
                    track->close();
                    self->report_error("Unexpected remote-created WebRTC media track");
                    return;
                }
                self->attach_video_track(std::move(track), true);
            }
        });
    }

    void start() {
        if (closing.load()) throw std::logic_error("WebRTC transport is closed");
        if (started.exchange(true)) throw std::logic_error("WebRTC transport is already started");
        if (configuration.role == Role::Offerer) {
            if (configuration.video_direction == VideoDirection::ReceiveOnly) {
                rtc::Description::Video media(
                    "video", rtc::Description::Direction::RecvOnly);
                if (configuration.video_codec == VideoCodec::H264) {
                    media.addH264Codec(96);
                } else {
                    media.addAV1Codec(96);
                }
                attach_video_track(peer_connection->addTrack(std::move(media)), false);
            }
            attach_data_channel(peer_connection->createDataChannel(configuration.data_channel_label));
        }
    }

    void attach_data_channel(std::shared_ptr<rtc::DataChannel> channel) {
        bool already_attached = false;
        {
            std::lock_guard lock(channel_mutex);
            if (data_channel && !data_channel->isClosed()) {
                already_attached = true;
            } else {
                data_channel = channel;
            }
        }
        if (already_attached) {
            channel->close();
            report_error("WebRTC transport already has a data channel");
            return;
        }

        const std::weak_ptr<Impl> weak_self = weak_from_this();
        channel->onOpen([weak_self] {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                self->open.store(true);
                invoke_callback(self->callbacks.on_open);
            }
        });
        channel->onClosed([weak_self] {
            if (const auto self = weak_self.lock()) {
                self->open.store(false);
                self->notify_closed();
            }
        });
        channel->onError([weak_self](std::string error) {
            if (const auto self = weak_self.lock()) self->report_error(std::move(error));
        });
        channel->onMessage(
            [weak_self](rtc::binary data) {
                if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                    std::vector<std::uint8_t> message(data.size());
                    if (!data.empty()) std::memcpy(message.data(), data.data(), data.size());
                    invoke_callback(self->callbacks.on_binary, std::move(message));
                }
            },
            [weak_self](std::string data) {
                if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                    invoke_callback(self->callbacks.on_text, std::move(data));
                }
            });

        if (channel->isOpen() && !closing.load()) {
            open.store(true);
            invoke_callback(callbacks.on_open);
        }
    }

    void attach_video_track(std::shared_ptr<rtc::Track> track, bool sending) {
        auto description = track->description();
        const auto payload_type = find_payload_type(description, configuration.video_codec);
        if (!payload_type) {
            track->close();
            report_error("Peer did not offer the configured " +
                         std::string(codec_name(configuration.video_codec)) + " video codec");
            return;
        }

        bool already_attached = false;
        {
            std::lock_guard lock(channel_mutex);
            if (video_track && !video_track->isClosed()) already_attached = true;
            else video_track = track;
        }
        if (already_attached) {
            track->close();
            report_error("WebRTC transport already has a video track");
            return;
        }

        if (sending) {
            for (const int candidate : description.payloadTypes()) {
                if (candidate != *payload_type) description.removeRtpMap(candidate);
            }
            const std::uint32_t ssrc = random_ssrc();
            description.clearSSRCs();
            description.addSSRC(ssrc, "remoe-video", "remoe-stream", "remoe-video");
            track->setDescription(std::move(description));

            auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(
                ssrc, "remoe-video", *payload_type, rtc::RtpPacketizer::VideoClockRate);
            // Preserve the sender's monotonic timestamp epoch in RTP rather than
            // adding libdatachannel's random offset. The receiver unwraps the
            // 32-bit RTP clock across long sessions.
            rtp->startTimestamp = 0;
            rtp->timestamp = 0;
            std::shared_ptr<rtc::MediaHandler> packetizer;
            if (configuration.video_codec == VideoCodec::H264) {
                packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                    rtc::NalUnit::Separator::StartSequence, rtp);
            } else {
                packetizer = std::make_shared<rtc::AV1RtpPacketizer>(
                    rtc::AV1RtpPacketizer::Packetization::TemporalUnit, rtp);
            }
            packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
            // Fixed-quality screen changes can produce multi-megabyte AV1
            // frames. Keep enough RTP history for the receiver's delayed NACKs
            // instead of evicting the beginning of the burst after 512 packets.
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(8192));
            const std::weak_ptr<Impl> weak_self = weak_from_this();
            packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak_self] {
                if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                    invoke_callback(self->callbacks.on_video_keyframe_requested);
                }
            }));
            track->setMediaHandler(std::move(packetizer));
        } else {
            auto receiver = std::make_shared<rtc::RtcpReceivingSession>();
            std::shared_ptr<rtc::MediaHandler> depacketizer;
            const std::weak_ptr<Impl> weak_self = weak_from_this();
            if (configuration.video_codec == VideoCodec::H264) {
                depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
                    rtc::NalUnit::Separator::StartSequence);
            } else {
                depacketizer = std::make_shared<Av1RtpDepacketizer>(
                    [weak_self] {
                        if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                            if (const auto video = self->current_video_track();
                                video && video->isOpen()) {
                                (void)video->requestKeyframe();
                            }
                        }
                    },
                    [weak_self](std::string message) {
                        if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                            invoke_callback(self->callbacks.on_diagnostic, std::move(message));
                        }
                    });
            }
            // Incoming handler chains execute in reverse order: RTCP/RTP
            // accounting must see the packet before depacketization.
            depacketizer->addToChain(std::move(receiver));
            track->setMediaHandler(std::move(depacketizer));
            track->onFrame([weak_self](rtc::binary data, rtc::FrameInfo info) {
                if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                    std::vector<std::uint8_t> frame(data.size());
                    if (!data.empty()) std::memcpy(frame.data(), data.data(), data.size());
                    std::uint64_t extended_timestamp = info.timestamp;
                    {
                        std::lock_guard lock(self->video_timestamp_mutex);
                        if (self->have_video_timestamp && info.timestamp < self->last_video_timestamp &&
                            self->last_video_timestamp - info.timestamp > 0x80000000u) {
                            self->video_timestamp_wraps += (std::uint64_t{1} << 32);
                        }
                        self->last_video_timestamp = info.timestamp;
                        self->have_video_timestamp = true;
                        extended_timestamp += self->video_timestamp_wraps;
                    }
                    const std::uint64_t timestamp_us = extended_timestamp * 1'000'000u /
                        rtc::RtpPacketizer::VideoClockRate;
                    const bool key_frame = encoded_key_frame(
                        self->configuration.video_codec, frame);
                    invoke_callback(self->callbacks.on_video_frame,
                                    std::move(frame), timestamp_us, key_frame);
                }
            });
        }

        const std::weak_ptr<Impl> weak_self = weak_from_this();
        track->onOpen([weak_self] {
            if (const auto self = weak_self.lock(); self && !self->closing.load()) {
                self->video_open.store(true);
                invoke_callback(self->callbacks.on_video_open);
            }
        });
        track->onClosed([weak_self] {
            if (const auto self = weak_self.lock()) self->video_open.store(false);
        });
        track->onError([weak_self](std::string error) {
            if (const auto self = weak_self.lock()) self->report_error(std::move(error));
        });
        if (track->isOpen() && !closing.load()) {
            video_open.store(true);
            invoke_callback(callbacks.on_video_open);
        }
    }

    std::shared_ptr<rtc::DataChannel> current_channel() const {
        std::lock_guard lock(channel_mutex);
        return data_channel;
    }

    std::shared_ptr<rtc::Track> current_video_track() const {
        std::lock_guard lock(channel_mutex);
        return video_track;
    }

    void report_error(std::string error) const noexcept {
        invoke_callback(callbacks.on_error, std::move(error));
    }

    void notify_closed() noexcept {
        if (!closed_notified.exchange(true)) invoke_callback(callbacks.on_closed);
    }

    void close() noexcept {
        if (closing.exchange(true)) return;
        open.store(false);
        video_open.store(false);

        try {
            if (const auto channel = current_channel()) {
                channel->resetCallbacks();
                channel->close();
            }
            if (const auto track = current_video_track()) {
                track->resetCallbacks();
                track->close();
            }
            if (peer_connection) {
                peer_connection->resetCallbacks();
                peer_connection->close();
            }
        } catch (...) {
        }

        connection_state.store(State::Closed);
        current_ice_state.store(IceState::Closed);
        notify_closed();
    }
};

WebRtcTransport::WebRtcTransport(Configuration configuration, Callbacks callbacks)
    : impl_(std::make_shared<Impl>(std::move(configuration), std::move(callbacks))) {
    impl_->initialize();
}

WebRtcTransport::~WebRtcTransport() {
    close();
}

void WebRtcTransport::start() {
    impl_->start();
}

void WebRtcTransport::set_remote_description(std::string_view sdp, std::string_view type) {
    if (sdp.empty()) throw std::invalid_argument("Remote WebRTC SDP must not be empty");
    if (type.empty()) throw std::invalid_argument("Remote WebRTC SDP type must not be empty");
    if (impl_->closing.load()) throw std::logic_error("WebRTC transport is closed");
    impl_->peer_connection->setRemoteDescription(
        rtc::Description(std::string(sdp), std::string(type)));
}

void WebRtcTransport::add_remote_candidate(std::string_view candidate, std::string_view mid) {
    if (candidate.empty()) throw std::invalid_argument("Remote WebRTC ICE candidate must not be empty");
    if (impl_->closing.load()) throw std::logic_error("WebRTC transport is closed");
    impl_->peer_connection->addRemoteCandidate(
        rtc::Candidate(std::string(candidate), std::string(mid)));
}

bool WebRtcTransport::send_text(std::string_view message) noexcept {
    try {
        const auto channel = impl_->current_channel();
        if (!channel || !channel->isOpen()) return false;
        (void)channel->send(std::string(message));
        return true;
    } catch (const std::exception& error) {
        impl_->report_error(error.what());
        return false;
    }
}

bool WebRtcTransport::send_binary(std::span<const std::uint8_t> message) noexcept {
    try {
        const auto channel = impl_->current_channel();
        if (!channel || !channel->isOpen()) return false;
        (void)channel->send(
            reinterpret_cast<const rtc::byte*>(message.data()), message.size());
        return true;
    } catch (const std::exception& error) {
        impl_->report_error(error.what());
        return false;
    }
}

bool WebRtcTransport::send_video_frame(std::span<const std::uint8_t> frame,
                                       std::uint64_t timestamp_us) noexcept {
    try {
        const auto track = impl_->current_video_track();
        if (!track || !track->isOpen() ||
            impl_->configuration.video_direction != VideoDirection::SendOnly) return false;
        track->sendFrame(reinterpret_cast<const rtc::byte*>(frame.data()), frame.size(),
            rtc::FrameInfo(std::chrono::duration<double, std::micro>(timestamp_us)));
        return true;
    } catch (const std::exception& error) {
        impl_->report_error(error.what());
        return false;
    }
}

bool WebRtcTransport::request_video_keyframe() noexcept {
    try {
        const auto track = impl_->current_video_track();
        return track && track->isOpen() &&
            impl_->configuration.video_direction == VideoDirection::ReceiveOnly &&
            track->requestKeyframe();
    } catch (const std::exception& error) {
        impl_->report_error(error.what());
        return false;
    }
}

void WebRtcTransport::close() noexcept {
    if (impl_) impl_->close();
}

bool WebRtcTransport::is_open() const noexcept {
    return impl_ && impl_->open.load();
}

WebRtcTransport::State WebRtcTransport::state() const noexcept {
    return impl_ ? impl_->connection_state.load() : State::Closed;
}

WebRtcTransport::IceState WebRtcTransport::ice_state() const noexcept {
    return impl_ ? impl_->current_ice_state.load() : IceState::Closed;
}

WebRtcTransport::GatheringState WebRtcTransport::gathering_state() const noexcept {
    return impl_ ? impl_->current_gathering_state.load() : GatheringState::New;
}

std::size_t WebRtcTransport::buffered_amount() const noexcept {
    if (!impl_) return 0;
    try {
        const auto channel = impl_->current_channel();
        return channel ? channel->bufferedAmount() : 0;
    } catch (...) {
        return 0;
    }
}

bool WebRtcTransport::is_video_open() const noexcept {
    return impl_ && impl_->video_open.load();
}

WebRtcTransport::Statistics WebRtcTransport::statistics() const {
    Statistics result;
    if (!impl_ || !impl_->peer_connection) return result;

    result.bytes_sent = impl_->peer_connection->bytesSent();
    result.bytes_received = impl_->peer_connection->bytesReceived();
    result.round_trip_time = impl_->peer_connection->rtt();

    rtc::Candidate local;
    rtc::Candidate remote;
    if (impl_->peer_connection->getSelectedCandidatePair(&local, &remote)) {
        result.local_candidate = to_public_candidate(local);
        result.remote_candidate = to_public_candidate(remote);
    }
    return result;
}

} // namespace remoe
