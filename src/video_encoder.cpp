#include "video_encoder.h"

#include <exception>
#include <iostream>
#include <memory>

namespace remoe {

#if defined(REMOE_HAS_VPL_ENCODER)
std::unique_ptr<Av1Encoder> create_vpl_av1_encoder(
    ID3D11Device*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
#endif
std::unique_ptr<Av1Encoder> create_nvenc_av1_encoder(
    ID3D11Device*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);

std::unique_ptr<Av1Encoder> create_preferred_av1_encoder(
    ID3D11Device* device, std::uint32_t width, std::uint32_t height,
    std::uint32_t fps, std::uint32_t bitrate_bps) {
#if defined(REMOE_HAS_VPL_ENCODER)
    try {
        auto encoder = create_vpl_av1_encoder(device, width, height, fps, bitrate_bps);
        std::cout << "Selected AV1 encoder: " << encoder->name() << '\n';
        return encoder;
    } catch (const std::exception& error) {
        std::cerr << "Intel oneVPL AV1 unavailable: " << error.what()
                  << "\nFalling back to NVIDIA NVENC AV1\n";
    }
#else
    std::cerr << "Intel oneVPL AV1 was not included in this build; "
                 "falling back to NVIDIA NVENC AV1\n";
#endif

    auto encoder = create_nvenc_av1_encoder(device, width, height, fps, bitrate_bps);
    std::cout << "Selected AV1 encoder: " << encoder->name() << '\n';
    return encoder;
}

} // namespace remoe
