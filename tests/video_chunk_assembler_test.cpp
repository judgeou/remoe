#include "video_chunk_assembler.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

std::vector<std::uint8_t> make_chunk(std::uint64_t frame_number,
                                     std::uint64_t timestamp_us,
                                     std::span<const std::uint8_t> frame,
                                     std::size_t offset, bool key_frame = false) {
    remoe::protocol::VideoChunkHeader header;
    header.flags = key_frame ? remoe::protocol::kFrameKey : 0;
    header.frame_number = frame_number;
    header.timestamp_us = timestamp_us;
    header.frame_size = static_cast<std::uint32_t>(frame.size());
    header.chunk_offset = static_cast<std::uint32_t>(offset);
    const std::size_t size = (std::min)(
        remoe::protocol::kVideoChunkPayloadSize, frame.size() - offset);
    std::vector<std::uint8_t> message(sizeof(header) + size);
    std::memcpy(message.data(), &header, sizeof(header));
    std::memcpy(message.data() + sizeof(header), frame.data() + offset, size);
    return message;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        remoe::VideoChunkAssembler assembler;
        std::vector<std::uint8_t> source(
            remoe::protocol::kVideoChunkPayloadSize + 37);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<std::uint8_t>(index * 31u);
        }

        auto tail = make_chunk(7, 123456, source,
                               remoe::protocol::kVideoChunkPayloadSize, true);
        auto first = assembler.consume(tail);
        require(!first.frame, "an incomplete frame was emitted");
        auto head = make_chunk(7, 123456, source, 0, true);
        auto complete = assembler.consume(head);
        require(complete.frame.has_value(), "out-of-order chunks were not assembled");
        require(complete.frame->frame_number == 7 &&
                complete.frame->timestamp_us == 123456 && complete.frame->key_frame,
                "assembled frame metadata changed");
        require(complete.frame->payload == source, "assembled frame payload changed");

        std::vector<std::uint8_t> tiny{1, 2, 3};
        (void)assembler.consume(make_chunk(8, 8, source, 0));
        bool loss_detected = false;
        for (std::uint64_t number = 10; number <= 18; ++number) {
            auto result = assembler.consume(make_chunk(number, number, tiny, 0));
            loss_detected |= result.loss_detected;
        }
        require(loss_detected, "stale frame loss was not reported");

        assembler.clear();
        auto discarded = assembler.consume(make_chunk(18, 18, tiny, 0));
        require(!discarded.frame, "clear accepted a retired frame");

        remoe::LowLatencyVideoGate gate;
        auto missing_initial_key = gate.evaluate(remoe::ReassembledVideoFrame{
            tiny, 1, 1, false,
        });
        require(!missing_initial_key.frame && missing_initial_key.request_key_frame,
                "delta frame before the first key frame did not request recovery");
        auto recovered = gate.evaluate(remoe::ReassembledVideoFrame{
            tiny, 2, 2, true,
        });
        require(recovered.frame && recovered.reset_decoder,
                "recovery key frame did not reset the decoder");
        auto sequential = gate.evaluate(remoe::ReassembledVideoFrame{
            tiny, 3, 3, false,
        });
        require(sequential.frame && !sequential.request_key_frame,
                "sequential delta frame was rejected");
        auto gap = gate.evaluate(remoe::ReassembledVideoFrame{
            tiny, 5, 5, false,
        });
        require(!gap.frame && gap.request_key_frame,
                "frame-number gap did not request recovery");
        auto repeated_gap = gate.evaluate(remoe::ReassembledVideoFrame{
            tiny, 6, 6, false,
        });
        require(!repeated_gap.frame && !repeated_gap.request_key_frame,
                "recovery requested more than once before a key frame");

        std::cout << "Video chunk assembler tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Video chunk assembler tests failed: " << error.what() << '\n';
        return 1;
    }
}
