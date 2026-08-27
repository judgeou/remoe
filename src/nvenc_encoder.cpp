#include "video_encoder.h"

#include "NvEncoder/NvEncoderD3D11.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace remoe {
namespace {

std::uint32_t read_le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::span<const std::uint8_t> unwrap_av1_ivf(const std::vector<std::uint8_t>& packet) {
    std::size_t offset = 0;
    if (packet.size() >= 4 && packet[0] == 'D' && packet[1] == 'K' &&
        packet[2] == 'I' && packet[3] == 'F') {
        if (packet.size() < 32) {
            throw std::runtime_error("truncated IVF file header from NVENC wrapper");
        }
        offset = 32;
    }
    if (packet.size() < offset + 12) {
        throw std::runtime_error("truncated IVF frame header from NVENC wrapper");
    }
    const std::uint32_t frame_size = read_le32(packet.data() + offset);
    offset += 12;
    if (frame_size != packet.size() - offset) {
        throw std::runtime_error("invalid IVF frame size from NVENC wrapper");
    }
    return {packet.data() + offset, frame_size};
}

bool is_key_picture(NV_ENC_PIC_TYPE type) {
    return type == NV_ENC_PIC_TYPE_IDR || type == NV_ENC_PIC_TYPE_I ||
           type == NV_ENC_PIC_TYPE_SWITCH;
}

std::vector<EncodedVideoFrame> convert_packets(std::vector<NvEncOutputFrame>& packets) {
    std::vector<EncodedVideoFrame> frames;
    frames.reserve(packets.size());
    for (auto& packet : packets) {
        const auto av1 = unwrap_av1_ivf(packet.frame);
        EncodedVideoFrame frame;
        frame.data.assign(av1.begin(), av1.end());
        frame.key_frame = is_key_picture(packet.pictureType);
        frames.push_back(std::move(frame));
    }
    return frames;
}

class NvencAv1Encoder final : public Av1Encoder {
public:
    NvencAv1Encoder(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
                    std::uint32_t fps, std::uint32_t bitrate_bps,
                    protocol::VideoRateControl rate_control, std::uint32_t quality)
        : device_(device), encoder_(device, width, height, NV_ENC_BUFFER_FORMAT_ARGB, 0) {
        NV_ENC_INITIALIZE_PARAMS init{NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG config{NV_ENC_CONFIG_VER};
        init.encodeConfig = &config;
        encoder_.CreateDefaultEncoderParams(&init, NV_ENC_CODEC_AV1_GUID,
                                            NV_ENC_PRESET_P1_GUID,
                                            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);
        init.frameRateNum = fps;
        init.frameRateDen = 1;
        init.enablePTD = 1;
        init.encodeConfig->gopLength = NVENC_INFINITE_GOPLENGTH;
        init.encodeConfig->frameIntervalP = 1;
        if (rate_control == protocol::VideoRateControl::FixedQuality) {
            init.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            init.encodeConfig->rcParams.constQP.qpIntra = quality;
            init.encodeConfig->rcParams.constQP.qpInterP = quality;
            init.encodeConfig->rcParams.constQP.qpInterB = quality;
        } else {
            init.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            init.encodeConfig->rcParams.averageBitRate = bitrate_bps;
            init.encodeConfig->rcParams.maxBitRate = bitrate_bps;
            init.encodeConfig->rcParams.vbvBufferSize = bitrate_bps / fps;
            init.encodeConfig->rcParams.vbvInitialDelay =
                init.encodeConfig->rcParams.vbvBufferSize;
        }
        init.encodeConfig->encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        init.encodeConfig->encodeCodecConfig.av1Config.repeatSeqHdr = 1;
        encoder_.CreateEncoder(&init);
    }

    ~NvencAv1Encoder() override {
        try {
            encoder_.DestroyEncoder();
        } catch (...) {
        }
    }

    ID3D11Texture2D* input_texture() override {
        const NvEncInputFrame* input = encoder_.GetNextInputFrame();
        return static_cast<ID3D11Texture2D*>(input->inputPtr);
    }

    ID3D11Device* device() const noexcept override { return device_; }

    void discard_input() noexcept override {}

    std::vector<EncodedVideoFrame> encode(bool force_key_frame) override {
        NV_ENC_PIC_PARAMS picture{NV_ENC_PIC_PARAMS_VER};
        if (force_key_frame) {
            picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }
        std::vector<NvEncOutputFrame> packets;
        encoder_.EncodeFrame(packets, &picture);
        return convert_packets(packets);
    }

    std::vector<EncodedVideoFrame> drain() override {
        std::vector<NvEncOutputFrame> packets;
        encoder_.EndEncode(packets);
        return convert_packets(packets);
    }

    std::string_view name() const noexcept override { return "NVIDIA NVENC hardware AV1"; }

private:
    ID3D11Device* device_;
    NvEncoderD3D11 encoder_;
};

} // namespace

std::unique_ptr<Av1Encoder> create_nvenc_av1_encoder(
    ID3D11Device* device, std::uint32_t width, std::uint32_t height,
    std::uint32_t fps, std::uint32_t bitrate_bps,
    protocol::VideoRateControl rate_control, std::uint32_t quality) {
    return std::make_unique<NvencAv1Encoder>(
        device, width, height, fps, bitrate_bps, rate_control, quality);
}

} // namespace remoe
