#include "x264_encoder.h"

#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

bool contains_annex_b_nal(std::span<const std::uint8_t> bytes,
                          std::uint8_t wanted_type) {
    for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
        std::size_t header = 0;
        if (bytes[index] == 0 && bytes[index + 1] == 0 &&
            bytes[index + 2] == 1) {
            header = index + 3;
        } else if (index + 5 < bytes.size() && bytes[index] == 0 &&
                   bytes[index + 1] == 0 && bytes[index + 2] == 0 &&
                   bytes[index + 3] == 1) {
            header = index + 4;
        }
        if (header != 0 && (bytes[header] & 0x1fu) == wanted_type) return true;
    }
    return false;
}

} // namespace

int main() {
    try {
        constexpr std::uint32_t width = 640;
        constexpr std::uint32_t height = 360;
        constexpr std::uint32_t stride = width * 4;
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(stride) * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t offset = static_cast<std::size_t>(y) * stride + x * 4;
                bgra[offset] = static_cast<std::uint8_t>(x);
                bgra[offset + 1] = static_cast<std::uint8_t>(y);
                bgra[offset + 2] = static_cast<std::uint8_t>(x + y);
                bgra[offset + 3] = 255;
            }
        }

        auto encoder = remoe::create_x264_h264_encoder(width, height, 30, 2'000'000);
        if (encoder->profile_level_id() == 0) {
            throw std::runtime_error("missing H.264 profile-level-id");
        }
        auto frames = encoder->encode(bgra, width, height, stride, true);
        if (frames.size() != 1 || frames.front().data.empty() ||
            !frames.front().key_frame) {
            throw std::runtime_error("x264 did not return one IDR access unit");
        }
        const auto bitstream = std::span<const std::uint8_t>(frames.front().data);
        if (!contains_annex_b_nal(bitstream, 7) ||
            !contains_annex_b_nal(bitstream, 8) ||
            !contains_annex_b_nal(bitstream, 5)) {
            throw std::runtime_error("IDR access unit lacks SPS, PPS, or IDR NAL");
        }
        std::cout << "x264 encoder smoke test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "x264 encoder smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
