#pragma once

#include <d3d11.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace remoe {

class VplAv1Decoder {
public:
    using FrameCallback = std::function<void(ID3D11Texture2D*, std::uint32_t, std::uint32_t)>;

    VplAv1Decoder(ID3D11Device* device, FrameCallback callback);
    ~VplAv1Decoder();

    VplAv1Decoder(const VplAv1Decoder&) = delete;
    VplAv1Decoder& operator=(const VplAv1Decoder&) = delete;

    void submit(std::span<const std::uint8_t> encoded_frame);
    void drain();
    [[nodiscard]] const std::string& implementation_name() const noexcept {
        return implementation_name_;
    }

private:
    void initialize_decoder();
    void decode_available(bool draining);
    void handle_surface(mfxFrameSurface1* surface);
    void compact_bitstream();

    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxVideoParam parameters_{};
    mfxBitstream bitstream_{};
    std::vector<mfxU8> storage_;
    FrameCallback callback_;
    std::string implementation_name_;
    bool initialized_ = false;
};

} // namespace remoe
