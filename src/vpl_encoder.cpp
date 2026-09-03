#include "video_encoder.h"

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace remoe {
namespace {

std::runtime_error vpl_error(const char* operation, mfxStatus status) {
    return std::runtime_error(std::string(operation) + " failed, oneVPL status " +
                              std::to_string(status));
}

void check_vpl(mfxStatus status, const char* operation) {
    if (status < MFX_ERR_NONE) throw vpl_error(operation, status);
}

void set_filter(mfxLoader loader, const char* property, mfxU32 value) {
    mfxConfig config = MFXCreateConfig(loader);
    if (!config) throw std::runtime_error("MFXCreateConfig failed");
    mfxVariant variant{};
    variant.Type = MFX_VARIANT_TYPE_U32;
    variant.Data.U32 = value;
    check_vpl(MFXSetConfigFilterProperty(
                  config, reinterpret_cast<const mfxU8*>(property), variant),
              property);
}

void set_d3d11_import_filter(mfxLoader loader) {
    mfxConfig config = MFXCreateConfig(loader);
    if (!config) throw std::runtime_error("MFXCreateConfig failed");
    auto set_property = [&](const char* property, mfxU32 value) {
        mfxVariant variant{};
        variant.Type = MFX_VARIANT_TYPE_U32;
        variant.Data.U32 = value;
        check_vpl(MFXSetConfigFilterProperty(
                      config, reinterpret_cast<const mfxU8*>(property), variant),
                  property);
    };
    set_property("mfxSurfaceTypesSupported.surftype.SurfaceType",
                 MFX_SURFACE_TYPE_D3D11_TEX2D);
    set_property("mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceComponent",
                 MFX_SURFACE_COMPONENT_ENCODE);
    set_property("mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceFlags",
                 MFX_SURFACE_FLAG_IMPORT_SHARED);
}

std::optional<std::span<const std::uint8_t>> find_av1_sequence_header(
    std::span<const std::uint8_t> frame) {
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const std::size_t obu_start = offset;
        const std::uint8_t header = frame[offset++];
        if ((header & 0x80u) != 0 || (header & 0x01u) != 0) return std::nullopt;
        const std::uint8_t obu_type = (header >> 3) & 0x0fu;
        if ((header & 0x04u) != 0) {
            if (offset >= frame.size()) return std::nullopt;
            ++offset;
        }
        if ((header & 0x02u) == 0) return std::nullopt;

        std::uint64_t payload_size = 0;
        unsigned shift = 0;
        bool size_complete = false;
        for (int byte_index = 0; byte_index < 8 && offset < frame.size(); ++byte_index) {
            const std::uint8_t size_byte = frame[offset++];
            payload_size |= static_cast<std::uint64_t>(size_byte & 0x7fu) << shift;
            if ((size_byte & 0x80u) == 0) {
                size_complete = true;
                break;
            }
            shift += 7;
        }
        if (!size_complete || payload_size > frame.size() - offset) return std::nullopt;
        offset += static_cast<std::size_t>(payload_size);
        if (obu_type == 1) return frame.subspan(obu_start, offset - obu_start);
    }
    return std::nullopt;
}

class SurfaceGuard {
public:
    explicit SurfaceGuard(mfxFrameSurface1* surface) : surface_(surface) {}
    ~SurfaceGuard() { reset(); }
    SurfaceGuard(const SurfaceGuard&) = delete;
    SurfaceGuard& operator=(const SurfaceGuard&) = delete;
    void reset() {
        if (surface_ && surface_->FrameInterface) {
            surface_->FrameInterface->Release(surface_);
        }
        surface_ = nullptr;
    }

private:
    mfxFrameSurface1* surface_;
};

class VplAv1Encoder final : public Av1Encoder {
public:
    VplAv1Encoder(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
                  std::uint32_t fps, std::uint32_t bitrate_bps,
                  protocol::VideoRateControl rate_control, std::uint32_t quality)
        : width_(width), height_(height), fps_(fps), bitrate_bps_(bitrate_bps),
          rate_control_(rate_control) {
        loader_ = MFXLoad();
        if (!loader_) {
            throw std::runtime_error(
                "MFXLoad failed; install an Intel graphics driver with oneVPL runtime");
        }

        try {
            set_filter(loader_, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
            set_filter(loader_, "mfxImplDescription.VendorID", 0x8086);
            set_filter(loader_, "mfxImplDescription.AccelerationMode",
                       MFX_ACCEL_MODE_VIA_D3D11);
            set_filter(loader_,
                       "mfxImplDescription.mfxEncoderDescription.encoder.CodecID",
                       MFX_CODEC_AV1);
            set_filter(loader_, "mfxImplDescription.ApiVersion.Version", (2u << 16) | 10u);
            set_d3d11_import_filter(loader_);
            check_vpl(MFXCreateSession(loader_, 0, &session_),
                      "MFXCreateSession (Intel AV1 hardware encoder)");

            parameters_.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
            parameters_.AsyncDepth = 1;
            parameters_.mfx.CodecId = MFX_CODEC_AV1;
            parameters_.mfx.CodecProfile = MFX_PROFILE_AV1_MAIN;
            parameters_.mfx.LowPower = MFX_CODINGOPTION_ON;
            parameters_.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_SPEED;
            parameters_.mfx.RateControlMethod = static_cast<mfxU16>(
                rate_control == protocol::VideoRateControl::FixedQuality
                    ? MFX_RATECONTROL_ICQ : MFX_RATECONTROL_CBR);
            parameters_.mfx.GopPicSize = 0;
            parameters_.mfx.GopRefDist = 1;
            parameters_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
            parameters_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
            parameters_.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
            parameters_.mfx.FrameInfo.FrameRateExtN = fps;
            parameters_.mfx.FrameInfo.FrameRateExtD = 1;
            parameters_.mfx.FrameInfo.CropW = static_cast<mfxU16>(width);
            parameters_.mfx.FrameInfo.CropH = static_cast<mfxU16>(height);
            parameters_.mfx.FrameInfo.Width = static_cast<mfxU16>((width + 15u) & ~15u);
            parameters_.mfx.FrameInfo.Height = static_cast<mfxU16>((height + 15u) & ~15u);

            if (rate_control == protocol::VideoRateControl::FixedQuality) {
                parameters_.mfx.ICQQuality = static_cast<mfxU16>(quality);
            } else {
                apply_cbr_parameters(parameters_, bitrate_bps, fps);
            }

            mfxStatus status = MFXVideoENCODE_Query(session_, &parameters_, &parameters_);
            if (status != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
                check_vpl(status, "MFXVideoENCODE_Query(AV1)");
            }
            check_vpl(MFXVideoENCODE_Init(session_, &parameters_),
                      "MFXVideoENCODE_Init(AV1 hardware)");
            initialized_ = true;

            mfxHDL internal_device = nullptr;
            check_vpl(MFXVideoCORE_GetHandle(session_, MFX_HANDLE_D3D11_DEVICE,
                                             &internal_device),
                      "MFXVideoCORE_GetHandle(D3D11)");
            if (!internal_device) {
                throw std::runtime_error("oneVPL returned a null D3D11 device");
            }
            device_ = static_cast<ID3D11Device*>(internal_device);
            ensure_same_adapter(device, device_.Get());

            check_vpl(MFXGetMemoryInterface(session_, &memory_interface_),
                      "MFXGetMemoryInterface");
            if (!memory_interface_) {
                throw std::runtime_error("oneVPL returned a null memory interface");
            }

            D3D11_TEXTURE2D_DESC texture_description{};
            texture_description.Width = parameters_.mfx.FrameInfo.Width;
            texture_description.Height = parameters_.mfx.FrameInfo.Height;
            texture_description.MipLevels = 1;
            texture_description.ArraySize = 1;
            texture_description.Format = DXGI_FORMAT_NV12;
            texture_description.SampleDesc.Count = 1;
            texture_description.Usage = D3D11_USAGE_DEFAULT;
            texture_description.BindFlags = D3D11_BIND_DECODER |
                                            D3D11_BIND_VIDEO_ENCODER |
                                            D3D11_BIND_SHADER_RESOURCE;
            texture_description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            const HRESULT texture_status = device_->CreateTexture2D(
                &texture_description, nullptr, &input_texture_);
            if (FAILED(texture_status)) {
                throw std::runtime_error("failed to create shared NV12 oneVPL input texture");
            }
            texture_description.BindFlags = D3D11_BIND_RENDER_TARGET;
            texture_description.MiscFlags = 0;
            const HRESULT conversion_status = device_->CreateTexture2D(
                &texture_description, nullptr, &conversion_texture_);
            if (FAILED(conversion_status)) {
                throw std::runtime_error("failed to create NV12 capture conversion texture");
            }
            device_->GetImmediateContext(&context_);
            if (!context_) {
                throw std::runtime_error("failed to get oneVPL D3D11 device context");
            }

            // Import once during initialization to verify the advertised sharing path.
            import_input_surface();
            discard_input();

            mfxVersion version{};
            check_vpl(MFXQueryVersion(session_, &version), "MFXQueryVersion");
            name_ = "Intel oneVPL " + std::to_string(version.Major) + "." +
                    std::to_string(version.Minor) + " D3D11 hardware AV1";

            const std::uint64_t bytes_per_frame =
                (static_cast<std::uint64_t>(bitrate_bps) / 8u + fps - 1u) / fps;
            const std::uint64_t buffer_size = rate_control ==
                protocol::VideoRateControl::FixedQuality
                    ? 64u * 1024u * 1024u
                    : (std::max<std::uint64_t>)(8u * 1024u * 1024u,
                                               bytes_per_frame * 8u);
            if (buffer_size > (std::numeric_limits<mfxU32>::max)()) {
                throw std::runtime_error("requested bitrate needs an oversized oneVPL buffer");
            }
            storage_.resize(static_cast<std::size_t>(buffer_size));
            bitstream_.Data = storage_.data();
            bitstream_.MaxLength = static_cast<mfxU32>(storage_.size());
            bitstream_.CodecId = MFX_CODEC_AV1;
        } catch (...) {
            close();
            throw;
        }
    }

    ~VplAv1Encoder() override { close(); }

    ID3D11Texture2D* input_texture() override {
        if (input_surface_) {
            throw std::runtime_error("oneVPL encode surface was not submitted");
        }
        import_input_surface();
        return conversion_texture_.Get();
    }

    ID3D11Device* device() const noexcept override { return device_.Get(); }

    void discard_input() noexcept override {
        if (input_surface_ && input_surface_->FrameInterface) {
            input_surface_->FrameInterface->Release(input_surface_);
        }
        input_surface_ = nullptr;
    }

    std::vector<EncodedVideoFrame> encode(bool force_key_frame) override {
        if (!input_surface_) {
            throw std::runtime_error("oneVPL encode called without an input surface");
        }
        SurfaceGuard surface(input_surface_);
        mfxFrameSurface1* submitted_surface = input_surface_;
        input_surface_ = nullptr;

        context_->CopyResource(input_texture_.Get(), conversion_texture_.Get());
        context_->Flush();

        mfxEncodeCtrl control{};
        if (force_key_frame) {
            control.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        }
        return submit(force_key_frame ? &control : nullptr, submitted_surface, false);
    }

    std::vector<EncodedVideoFrame> drain() override {
        std::vector<EncodedVideoFrame> frames;
        for (;;) {
            auto output = submit(nullptr, nullptr, true);
            if (output.empty()) break;
            frames.insert(frames.end(),
                          std::make_move_iterator(output.begin()),
                          std::make_move_iterator(output.end()));
        }
        return frames;
    }

    bool reconfigure_bitrate(std::uint32_t bitrate_bps) override {
        if (rate_control_ != protocol::VideoRateControl::Cbr || bitrate_bps < 1'000'000u) {
            return false;
        }
        if (bitrate_bps == bitrate_bps_) return true;
        mfxVideoParam updated = parameters_;
        apply_cbr_parameters(updated, bitrate_bps, fps_);
        mfxStatus status = MFXVideoENCODE_Query(session_, &updated, &updated);
        if (status != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM) {
            check_vpl(status, "MFXVideoENCODE_Query(adaptive bitrate)");
        }
        check_vpl(MFXVideoENCODE_Reset(session_, &updated),
                  "MFXVideoENCODE_Reset(adaptive bitrate)");
        parameters_ = updated;
        bitrate_bps_ = bitrate_bps;
        return true;
    }

    std::string_view name() const noexcept override { return name_; }

private:
    static void apply_cbr_parameters(mfxVideoParam& parameters,
                                     std::uint32_t bitrate_bps,
                                     std::uint32_t fps) {
        const std::uint32_t target_kbps = (std::max)(1u, bitrate_bps / 1000u);
        const std::uint32_t multiplier =
            (target_kbps + (std::numeric_limits<mfxU16>::max)() - 1u) /
            (std::numeric_limits<mfxU16>::max)();
        parameters.mfx.BRCParamMultiplier = static_cast<mfxU16>(multiplier);
        parameters.mfx.TargetKbps = static_cast<mfxU16>(
            (target_kbps + multiplier - 1u) / multiplier);
        parameters.mfx.MaxKbps = parameters.mfx.TargetKbps;
        // oneVPL expresses this field in kilobytes (not kilobits). Keep one
        // frame of VBV while also accounting for BRCParamMultiplier.
        const auto buffer_denominator = static_cast<std::uint64_t>(fps) * 8u * multiplier;
        const auto buffer_size = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(target_kbps) + buffer_denominator - 1u) /
            buffer_denominator);
        parameters.mfx.BufferSizeInKB = static_cast<mfxU16>((std::min)(
            65535u, (std::max)(1u, buffer_size)));
        parameters.mfx.InitialDelayInKB = parameters.mfx.BufferSizeInKB;
    }

    void import_input_surface() {
        mfxSurfaceD3D11Tex2D external_surface{};
        external_surface.SurfaceInterface.Header.SurfaceType =
            MFX_SURFACE_TYPE_D3D11_TEX2D;
        external_surface.SurfaceInterface.Header.SurfaceFlags =
            MFX_SURFACE_FLAG_IMPORT_SHARED;
        external_surface.SurfaceInterface.Header.StructSize =
            sizeof(mfxSurfaceD3D11Tex2D);
        external_surface.texture2D = input_texture_.Get();
        check_vpl(memory_interface_->ImportFrameSurface(
                      memory_interface_, MFX_SURFACE_COMPONENT_ENCODE,
                      &external_surface.SurfaceInterface.Header, &input_surface_),
                  "mfxMemoryInterface::ImportFrameSurface(D3D11)");
    }

    static void ensure_same_adapter(ID3D11Device* capture_device,
                                    ID3D11Device* encoder_device) {
        Microsoft::WRL::ComPtr<IDXGIDevice> capture_dxgi;
        Microsoft::WRL::ComPtr<IDXGIDevice> encoder_dxgi;
        if (FAILED(capture_device->QueryInterface(IID_PPV_ARGS(&capture_dxgi))) ||
            FAILED(encoder_device->QueryInterface(IID_PPV_ARGS(&encoder_dxgi)))) {
            throw std::runtime_error("failed to query encoder DXGI adapter");
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter> capture_adapter;
        Microsoft::WRL::ComPtr<IDXGIAdapter> encoder_adapter;
        if (FAILED(capture_dxgi->GetAdapter(&capture_adapter)) ||
            FAILED(encoder_dxgi->GetAdapter(&encoder_adapter))) {
            throw std::runtime_error("failed to obtain encoder DXGI adapter");
        }
        DXGI_ADAPTER_DESC capture_description{};
        DXGI_ADAPTER_DESC encoder_description{};
        if (FAILED(capture_adapter->GetDesc(&capture_description)) ||
            FAILED(encoder_adapter->GetDesc(&encoder_description))) {
            throw std::runtime_error("failed to describe encoder DXGI adapter");
        }
        if (capture_description.AdapterLuid.HighPart !=
                encoder_description.AdapterLuid.HighPart ||
            capture_description.AdapterLuid.LowPart !=
                encoder_description.AdapterLuid.LowPart) {
            throw std::runtime_error(
                "Intel oneVPL device is not attached to the selected display output");
        }
    }

    std::vector<EncodedVideoFrame> submit(mfxEncodeCtrl* control,
                                          mfxFrameSurface1* surface,
                                          bool draining) {
        for (;;) {
            mfxSyncPoint sync = nullptr;
            const mfxStatus status = MFXVideoENCODE_EncodeFrameAsync(
                session_, control, surface, &bitstream_, &sync);
            if (status == MFX_WRN_DEVICE_BUSY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (status == MFX_ERR_MORE_DATA) return {};
            if (status == MFX_ERR_NOT_ENOUGH_BUFFER) {
                throw vpl_error("MFXVideoENCODE_EncodeFrameAsync(output buffer)", status);
            }
            if (status < MFX_ERR_NONE) {
                throw vpl_error("MFXVideoENCODE_EncodeFrameAsync", status);
            }
            if (!sync) {
                if (draining) return {};
                continue;
            }

            mfxStatus sync_status;
            do {
                sync_status = MFXVideoCORE_SyncOperation(session_, sync, 1000);
            } while (sync_status == MFX_WRN_IN_EXECUTION);
            check_vpl(sync_status, "MFXVideoCORE_SyncOperation(AV1 encode)");

            EncodedVideoFrame frame;
            frame.data.assign(bitstream_.Data + bitstream_.DataOffset,
                              bitstream_.Data + bitstream_.DataOffset + bitstream_.DataLength);
            frame.key_frame = (bitstream_.FrameType &
                               (MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR)) != 0;
            if (frame.key_frame) {
                if (const auto sequence = find_av1_sequence_header(frame.data)) {
                    sequence_header_.assign(sequence->begin(), sequence->end());
                } else if (!sequence_header_.empty()) {
                    frame.data.insert(frame.data.begin(), sequence_header_.begin(),
                                      sequence_header_.end());
                }
            }
            bitstream_.DataOffset = 0;
            bitstream_.DataLength = 0;
            return {std::move(frame)};
        }
    }

    void close() noexcept {
        if (input_surface_ && input_surface_->FrameInterface) {
            input_surface_->FrameInterface->Release(input_surface_);
            input_surface_ = nullptr;
        }
        if (initialized_ && session_) {
            MFXVideoENCODE_Close(session_);
            initialized_ = false;
        }
        if (session_) {
            MFXClose(session_);
            session_ = nullptr;
        }
        if (loader_) {
            MFXUnload(loader_);
            loader_ = nullptr;
        }
    }

    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t fps_;
    std::uint32_t bitrate_bps_;
    protocol::VideoRateControl rate_control_;
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    mfxVideoParam parameters_{};
    mfxBitstream bitstream_{};
    mfxMemoryInterface* memory_interface_ = nullptr;
    std::vector<mfxU8> storage_;
    std::vector<std::uint8_t> sequence_header_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_texture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> conversion_texture_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    mfxFrameSurface1* input_surface_ = nullptr;
    std::string name_;
    bool initialized_ = false;
};

} // namespace

std::unique_ptr<Av1Encoder> create_vpl_av1_encoder(
    ID3D11Device* device, std::uint32_t width, std::uint32_t height,
    std::uint32_t fps, std::uint32_t bitrate_bps,
    protocol::VideoRateControl rate_control, std::uint32_t quality) {
    return std::make_unique<VplAv1Encoder>(
        device, width, height, fps, bitrate_bps, rate_control, quality);
}

} // namespace remoe
