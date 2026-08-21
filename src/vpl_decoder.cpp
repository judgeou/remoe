#include "vpl_decoder.h"

#include <vpl/mfxdispatcher.h>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

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
    check_vpl(MFXSetConfigFilterProperty(config,
                                         reinterpret_cast<const mfxU8*>(property), variant),
              property);
}

} // namespace

VplAv1Decoder::VplAv1Decoder(ID3D11Device* device, FrameCallback callback)
    : storage_(4 * 1024 * 1024), callback_(std::move(callback)) {
    loader_ = MFXLoad();
    if (!loader_) {
        throw std::runtime_error("MFXLoad failed; install the Intel graphics driver/oneVPL runtime");
    }

    set_filter(loader_, "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
    set_filter(loader_, "mfxImplDescription.VendorID", 0x8086);
    set_filter(loader_, "mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_D3D11);
    set_filter(loader_, "mfxImplDescription.mfxDecoderDescription.decoder.CodecID", MFX_CODEC_AV1);
    set_filter(loader_, "mfxImplDescription.ApiVersion.Version", (2u << 16) | 2u);

    check_vpl(MFXCreateSession(loader_, 0, &session_),
              "MFXCreateSession (Intel AV1 hardware decoder)");
    check_vpl(MFXVideoCORE_SetHandle(session_, MFX_HANDLE_D3D11_DEVICE,
                                     reinterpret_cast<mfxHDL>(device)),
              "MFXVideoCORE_SetHandle(D3D11)");

    mfxVersion version{};
    check_vpl(MFXQueryVersion(session_, &version), "MFXQueryVersion");
    implementation_name_ = "Intel oneVPL " + std::to_string(version.Major) + "." +
                           std::to_string(version.Minor) + " D3D11 hardware AV1";

    bitstream_.Data = storage_.data();
    bitstream_.MaxLength = static_cast<mfxU32>(storage_.size());
    bitstream_.CodecId = MFX_CODEC_AV1;
}

VplAv1Decoder::~VplAv1Decoder() {
    if (initialized_) MFXVideoDECODE_Close(session_);
    if (session_) MFXClose(session_);
    if (loader_) MFXUnload(loader_);
}

void VplAv1Decoder::compact_bitstream() {
    if (bitstream_.DataOffset == 0) return;
    if (bitstream_.DataLength > 0) {
        std::memmove(storage_.data(), storage_.data() + bitstream_.DataOffset,
                     bitstream_.DataLength);
    }
    bitstream_.DataOffset = 0;
}

void VplAv1Decoder::submit(std::span<const std::uint8_t> encoded_frame) {
    if (encoded_frame.empty()) return;
    compact_bitstream();
    const std::size_t required = static_cast<std::size_t>(bitstream_.DataLength) + encoded_frame.size();
    if (required > (std::numeric_limits<mfxU32>::max)()) {
        throw std::runtime_error("AV1 bitstream buffer exceeds oneVPL limits");
    }
    if (required > storage_.size()) {
        storage_.resize((std::max)(required, storage_.size() * 2));
        bitstream_.Data = storage_.data();
        bitstream_.MaxLength = static_cast<mfxU32>(storage_.size());
    }
    std::memcpy(storage_.data() + bitstream_.DataLength, encoded_frame.data(),
                encoded_frame.size());
    bitstream_.DataLength += static_cast<mfxU32>(encoded_frame.size());
    bitstream_.DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;

    if (!initialized_) initialize_decoder();
    if (initialized_) decode_available(false);
}

void VplAv1Decoder::initialize_decoder() {
    parameters_ = {};
    parameters_.mfx.CodecId = MFX_CODEC_AV1;
    parameters_.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
    parameters_.AsyncDepth = 1;
    const mfxStatus status = MFXVideoDECODE_DecodeHeader(session_, &bitstream_, &parameters_);
    if (status == MFX_ERR_MORE_DATA) return;
    check_vpl(status, "MFXVideoDECODE_DecodeHeader(AV1)");
    parameters_.AsyncDepth = 1;
    check_vpl(MFXVideoDECODE_Init(session_, &parameters_), "MFXVideoDECODE_Init(AV1 hardware)");
    initialized_ = true;
}

void VplAv1Decoder::decode_available(bool draining) {
    for (;;) {
        mfxFrameSurface1* surface = nullptr;
        mfxSyncPoint sync = nullptr;
        const mfxStatus status = MFXVideoDECODE_DecodeFrameAsync(
            session_, draining ? nullptr : &bitstream_, nullptr, &surface, &sync);

        if (status == MFX_WRN_DEVICE_BUSY) {
            Sleep(1);
            continue;
        }
        if (status == MFX_ERR_MORE_DATA) break;
        if (status == MFX_ERR_MORE_SURFACE) continue;
        if (status < MFX_ERR_NONE) throw vpl_error("MFXVideoDECODE_DecodeFrameAsync", status);
        if (!surface) {
            if (status == MFX_ERR_NONE) continue;
            break;
        }
        handle_surface(surface);
    }
}

void VplAv1Decoder::handle_surface(mfxFrameSurface1* surface) {
    struct SurfaceGuard {
        mfxFrameSurface1* surface;
        ~SurfaceGuard() {
            if (surface && surface->FrameInterface) surface->FrameInterface->Release(surface);
        }
    } guard{surface};

    mfxStatus status;
    do {
        status = surface->FrameInterface->Synchronize(surface, 1000);
    } while (status == MFX_WRN_IN_EXECUTION);
    check_vpl(status, "mfxFrameSurfaceInterface::Synchronize");

    mfxHDL resource = nullptr;
    mfxResourceType resource_type{};
    check_vpl(surface->FrameInterface->GetNativeHandle(surface, &resource, &resource_type),
              "mfxFrameSurfaceInterface::GetNativeHandle");
    if (resource_type != MFX_RESOURCE_DX11_TEXTURE || !resource) {
        throw std::runtime_error("oneVPL returned a non-D3D11 decode surface");
    }
    const std::uint32_t width = surface->Info.CropW ? surface->Info.CropW : surface->Info.Width;
    const std::uint32_t height = surface->Info.CropH ? surface->Info.CropH : surface->Info.Height;
    callback_(static_cast<ID3D11Texture2D*>(resource), width, height);
}

void VplAv1Decoder::drain() {
    if (initialized_) decode_available(true);
}

} // namespace remoe
