#pragma once

#include "encoded_video_frame.h"
#include "protocol.h"

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace remoe {

class Av1Encoder {
public:
    virtual ~Av1Encoder() = default;

    Av1Encoder(const Av1Encoder&) = delete;
    Av1Encoder& operator=(const Av1Encoder&) = delete;

    [[nodiscard]] virtual ID3D11Texture2D* input_texture() = 0;
    [[nodiscard]] virtual ID3D11Device* device() const noexcept = 0;
    virtual void discard_input() noexcept = 0;
    virtual std::vector<EncodedVideoFrame> encode(bool force_key_frame) = 0;
    virtual std::vector<EncodedVideoFrame> drain() = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    Av1Encoder() = default;
};

// Tries Intel oneVPL hardware AV1 first. If no compatible Intel implementation
// can initialize on this D3D11 device, falls back to NVIDIA NVENC AV1.
std::unique_ptr<Av1Encoder> create_preferred_av1_encoder(
    ID3D11Device* device, std::uint32_t width, std::uint32_t height,
    std::uint32_t fps, std::uint32_t bitrate_bps,
    protocol::VideoRateControl rate_control = protocol::VideoRateControl::Cbr,
    std::uint32_t quality = 28);

} // namespace remoe
