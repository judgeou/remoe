#pragma once

#include "protocol.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace remoe {

struct ReassembledVideoFrame {
    std::vector<std::uint8_t> payload;
    std::uint64_t frame_number = 0;
    std::uint64_t timestamp_us = 0;
    bool key_frame = false;
};

class VideoChunkAssembler {
public:
    struct Result {
        std::optional<ReassembledVideoFrame> frame;
        bool loss_detected = false;
    };

    Result consume(std::span<const std::uint8_t> message) {
        if (message.size() <= sizeof(protocol::VideoChunkHeader)) {
            throw std::runtime_error("host sent a truncated video chunk");
        }
        protocol::VideoChunkHeader header;
        std::memcpy(&header, message.data(), sizeof(header));
        const std::size_t payload_size = message.size() - sizeof(header);
        const std::uint64_t chunk_end =
            static_cast<std::uint64_t>(header.chunk_offset) + payload_size;
        if (header.magic != protocol::kVideoChunkMagic ||
            header.version != protocol::kVersion ||
            header.header_size != sizeof(header) || header.frame_size == 0 ||
            header.frame_size > 64u * 1024u * 1024u ||
            header.chunk_offset % protocol::kVideoChunkPayloadSize != 0 ||
            payload_size > protocol::kVideoChunkPayloadSize ||
            chunk_end > header.frame_size ||
            (chunk_end != header.frame_size &&
             payload_size != protocol::kVideoChunkPayloadSize) ||
            (header.flags & ~protocol::kFrameKey) != 0) {
            throw std::runtime_error("host sent an invalid video chunk");
        }

        if (!newest_ || header.frame_number > *newest_) newest_ = header.frame_number;
        if (discard_through_ && header.frame_number <= *discard_through_) return {};

        const std::size_t chunk_count =
            (header.frame_size + protocol::kVideoChunkPayloadSize - 1) /
            protocol::kVideoChunkPayloadSize;
        auto [found, inserted] = frames_.try_emplace(header.frame_number);
        Assembly& assembly = found->second;
        if (inserted) {
            assembly.flags = header.flags;
            assembly.timestamp_us = header.timestamp_us;
            assembly.data.resize(header.frame_size);
            assembly.received.resize(chunk_count);
        } else if (assembly.data.size() != header.frame_size ||
                   assembly.flags != header.flags ||
                   assembly.timestamp_us != header.timestamp_us) {
            throw std::runtime_error("video chunks for one frame disagree");
        }

        const std::size_t index = header.chunk_offset / protocol::kVideoChunkPayloadSize;
        if (!assembly.received[index]) {
            std::memcpy(assembly.data.data() + header.chunk_offset,
                        message.data() + sizeof(header), payload_size);
            assembly.received[index] = true;
            ++assembly.received_count;
        }

        Result result;
        bool current_retired = false;
        for (auto iterator = frames_.begin(); iterator != frames_.end();) {
            if (*newest_ > iterator->first && *newest_ - iterator->first > 8) {
                current_retired |= iterator->first == header.frame_number;
                iterator = frames_.erase(iterator);
                result.loss_detected = true;
            } else {
                ++iterator;
            }
        }
        if (current_retired) return result;

        found = frames_.find(header.frame_number);
        if (found == frames_.end() ||
            found->second.received_count != found->second.received.size()) return result;
        result.frame = ReassembledVideoFrame{
            std::move(found->second.data), header.frame_number, header.timestamp_us,
            (header.flags & protocol::kFrameKey) != 0,
        };
        frames_.erase(found);
        return result;
    }

    void clear() noexcept {
        frames_.clear();
        discard_through_ = newest_;
    }

private:
    struct Assembly {
        std::vector<std::uint8_t> data;
        std::vector<bool> received;
        std::size_t received_count = 0;
        std::uint64_t timestamp_us = 0;
        std::uint32_t flags = 0;
    };

    std::unordered_map<std::uint64_t, Assembly> frames_;
    std::optional<std::uint64_t> newest_;
    std::optional<std::uint64_t> discard_through_;
};

class LowLatencyVideoGate {
public:
    struct Decision {
        std::optional<ReassembledVideoFrame> frame;
        bool request_key_frame = false;
        bool reset_decoder = false;
    };

    Decision evaluate(std::optional<ReassembledVideoFrame> frame,
                      bool loss_detected = false) {
        Decision decision;
        const auto begin_recovery = [&] {
            waiting_for_key_frame_ = true;
            reset_on_key_frame_ = true;
            if (!recovery_requested_) {
                recovery_requested_ = true;
                decision.request_key_frame = true;
            }
        };
        if (loss_detected) begin_recovery();
        if (!frame || (last_frame_ && frame->frame_number <= *last_frame_)) return decision;
        if (last_frame_ && frame->frame_number != *last_frame_ + 1 &&
            !waiting_for_key_frame_) {
            begin_recovery();
        }
        last_frame_ = frame->frame_number;
        if (waiting_for_key_frame_ && !frame->key_frame) {
            begin_recovery();
            return decision;
        }
        if (frame->key_frame) {
            waiting_for_key_frame_ = false;
            recovery_requested_ = false;
            decision.request_key_frame = false;
            decision.reset_decoder = reset_on_key_frame_;
            reset_on_key_frame_ = false;
        }
        decision.frame = std::move(frame);
        return decision;
    }

private:
    std::optional<std::uint64_t> last_frame_;
    bool waiting_for_key_frame_ = true;
    bool recovery_requested_ = false;
    bool reset_on_key_frame_ = false;
};

} // namespace remoe
