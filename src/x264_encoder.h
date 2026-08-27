#pragma once

#include "encoded_video_frame.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace remoe {

class SoftwareH264Encoder {
public:
    virtual ~SoftwareH264Encoder() = default;

    SoftwareH264Encoder(const SoftwareH264Encoder&) = delete;
    SoftwareH264Encoder& operator=(const SoftwareH264Encoder&) = delete;

    virtual std::vector<EncodedVideoFrame> encode(
        std::span<const std::uint8_t> bgra, std::uint32_t source_width,
        std::uint32_t source_height, std::uint32_t source_stride,
        bool force_key_frame) = 0;
    virtual std::vector<EncodedVideoFrame> drain() = 0;
    [[nodiscard]] virtual std::uint32_t profile_level_id() const noexcept = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    SoftwareH264Encoder() = default;
};

std::unique_ptr<SoftwareH264Encoder> create_x264_h264_encoder(
    std::uint32_t width, std::uint32_t height, std::uint32_t fps,
    std::uint32_t bitrate_bps);

} // namespace remoe

