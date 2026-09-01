#include "x264_encoder.h"

#include <libyuv.h>
#include <x264.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace remoe {
namespace {

class X264H264Encoder final : public SoftwareH264Encoder {
public:
    X264H264Encoder(std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                    std::uint32_t bitrate_bps)
        : width_(width), height_(height), fps_(fps), bitrate_bps_(bitrate_bps) {
        if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0 ||
            fps == 0 || bitrate_bps < 1000) {
            throw std::runtime_error("invalid x264 encoder dimensions or rate");
        }

        x264_param_t parameters{};
        if (x264_param_default_preset(&parameters, "ultrafast", "zerolatency") < 0) {
            throw std::runtime_error("x264_param_default_preset failed");
        }
        parameters.i_csp = X264_CSP_I420;
        parameters.i_width = static_cast<int>(width);
        parameters.i_height = static_cast<int>(height);
        parameters.i_fps_num = static_cast<std::uint32_t>(fps);
        parameters.i_fps_den = 1;
        parameters.i_timebase_num = 1;
        parameters.i_timebase_den = static_cast<std::uint32_t>(fps);
        parameters.b_vfr_input = 0;
        parameters.b_repeat_headers = 1;
        parameters.b_annexb = 1;
        parameters.i_keyint_max = X264_KEYINT_MAX_INFINITE;
        parameters.i_keyint_min = X264_KEYINT_MIN_AUTO;
        parameters.i_scenecut_threshold = 0;
        parameters.i_bframe = 0;
        // -1 asks x264 to choose the lowest level that satisfies the stream.
        parameters.i_level_idc = -1;
        parameters.vui.b_fullrange = 0;
        parameters.vui.i_colorprim = 6;
        parameters.vui.i_transfer = 6;
        parameters.vui.i_colmatrix = 6;
        parameters.rc.i_rc_method = X264_RC_ABR;
        parameters.rc.i_bitrate = static_cast<int>(bitrate_bps / 1000);
        parameters.rc.i_vbv_max_bitrate = parameters.rc.i_bitrate;
        parameters.rc.i_vbv_buffer_size = (std::max)(
            parameters.rc.i_bitrate / static_cast<int>(fps), 1);
        if (x264_param_apply_profile(&parameters, "baseline") < 0) {
            throw std::runtime_error("x264_param_apply_profile(baseline) failed");
        }

        if (x264_picture_alloc(&input_, X264_CSP_I420, static_cast<int>(width),
                               static_cast<int>(height)) < 0) {
            throw std::runtime_error("x264_picture_alloc failed");
        }
        picture_allocated_ = true;
        encoder_ = x264_encoder_open(&parameters);
        if (!encoder_) {
            x264_picture_clean(&input_);
            picture_allocated_ = false;
            throw std::runtime_error("x264_encoder_open failed");
        }
        try {
            profile_level_id_ = read_profile_level_id();
        } catch (...) {
            x264_encoder_close(encoder_);
            encoder_ = nullptr;
            x264_picture_clean(&input_);
            picture_allocated_ = false;
            throw;
        }
    }

    ~X264H264Encoder() override {
        if (encoder_) x264_encoder_close(encoder_);
        if (picture_allocated_) x264_picture_clean(&input_);
    }

    std::vector<EncodedVideoFrame> encode(
        std::span<const std::uint8_t> bgra, std::uint32_t source_width,
        std::uint32_t source_height, std::uint32_t source_stride,
        bool force_key_frame) override {
        const std::size_t required = static_cast<std::size_t>(source_stride) * source_height;
        if (source_width == 0 || source_height == 0 || source_stride < source_width * 4 ||
            bgra.size() < required ||
            source_width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
            source_height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
            source_stride > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
            throw std::runtime_error("invalid BGRA capture frame for x264");
        }

        const std::uint8_t* conversion_source = bgra.data();
        int conversion_stride = static_cast<int>(source_stride);
        if (source_width != width_ || source_height != height_) {
            scaled_bgra_.resize(static_cast<std::size_t>(width_) * height_ * 4);
            if (libyuv::ARGBScale(
                    bgra.data(), static_cast<int>(source_stride),
                    static_cast<int>(source_width), static_cast<int>(source_height),
                    scaled_bgra_.data(), static_cast<int>(width_ * 4),
                    static_cast<int>(width_), static_cast<int>(height_),
                    libyuv::kFilterBilinear) != 0) {
                throw std::runtime_error("libyuv ARGBScale failed");
            }
            conversion_source = scaled_bgra_.data();
            conversion_stride = static_cast<int>(width_ * 4);
        }
        if (libyuv::ARGBToI420(
                conversion_source, conversion_stride,
                input_.img.plane[0], input_.img.i_stride[0],
                input_.img.plane[1], input_.img.i_stride[1],
                input_.img.plane[2], input_.img.i_stride[2],
                static_cast<int>(width_), static_cast<int>(height_)) != 0) {
            throw std::runtime_error("libyuv ARGBToI420 failed");
        }

        input_.i_pts = next_pts_++;
        input_.i_type = force_key_frame ? X264_TYPE_IDR : X264_TYPE_AUTO;
        return encode_picture(&input_);
    }

    std::vector<EncodedVideoFrame> drain() override {
        std::vector<EncodedVideoFrame> frames;
        while (encoder_ && x264_encoder_delayed_frames(encoder_) > 0) {
            auto delayed = encode_picture(nullptr);
            frames.insert(frames.end(), std::make_move_iterator(delayed.begin()),
                          std::make_move_iterator(delayed.end()));
        }
        return frames;
    }

    bool reconfigure_bitrate(std::uint32_t bitrate_bps) override {
        if (!encoder_ || bitrate_bps < 1'000'000u) return false;
        if (bitrate_bps == bitrate_bps_) return true;
        x264_param_t parameters{};
        x264_encoder_parameters(encoder_, &parameters);
        parameters.rc.i_bitrate = static_cast<int>(bitrate_bps / 1000u);
        parameters.rc.i_vbv_max_bitrate = parameters.rc.i_bitrate;
        parameters.rc.i_vbv_buffer_size = (std::max)(
            parameters.rc.i_bitrate / static_cast<int>(fps_), 1);
        if (x264_encoder_reconfig(encoder_, &parameters) < 0) return false;
        bitrate_bps_ = bitrate_bps;
        return true;
    }

    std::uint32_t profile_level_id() const noexcept override {
        return profile_level_id_;
    }

    std::string_view name() const noexcept override {
        return "x264 software H.264 constrained baseline";
    }

private:
    std::uint32_t read_profile_level_id() {
        x264_nal_t* nals = nullptr;
        int nal_count = 0;
        if (x264_encoder_headers(encoder_, &nals, &nal_count) < 0) {
            throw std::runtime_error("x264_encoder_headers failed");
        }
        for (int nal_index = 0; nal_index < nal_count; ++nal_index) {
            const x264_nal_t& nal = nals[nal_index];
            if (nal.i_type != NAL_SPS || nal.i_payload < 5) continue;
            int offset = 0;
            while (offset < nal.i_payload && nal.p_payload[offset] == 0) ++offset;
            if (offset >= nal.i_payload || nal.p_payload[offset] != 1) continue;
            ++offset;
            if (offset + 3 >= nal.i_payload || (nal.p_payload[offset] & 0x1fu) != NAL_SPS) {
                continue;
            }
            return (static_cast<std::uint32_t>(nal.p_payload[offset + 1]) << 16) |
                   (static_cast<std::uint32_t>(nal.p_payload[offset + 2]) << 8) |
                   static_cast<std::uint32_t>(nal.p_payload[offset + 3]);
        }
        throw std::runtime_error("x264 did not return a usable SPS");
    }

    std::vector<EncodedVideoFrame> encode_picture(x264_picture_t* picture) {
        x264_nal_t* nals = nullptr;
        int nal_count = 0;
        x264_picture_t output{};
        const int encoded_size = x264_encoder_encode(
            encoder_, &nals, &nal_count, picture, &output);
        if (encoded_size < 0) throw std::runtime_error("x264_encoder_encode failed");
        if (encoded_size == 0) return {};

        EncodedVideoFrame frame;
        frame.data.reserve(static_cast<std::size_t>(encoded_size));
        for (int index = 0; index < nal_count; ++index) {
            frame.data.insert(frame.data.end(), nals[index].p_payload,
                              nals[index].p_payload + nals[index].i_payload);
        }
        frame.key_frame = IS_X264_TYPE_I(output.i_type);
        return {std::move(frame)};
    }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t fps_ = 0;
    std::uint32_t bitrate_bps_ = 0;
    x264_t* encoder_ = nullptr;
    x264_picture_t input_{};
    bool picture_allocated_ = false;
    std::uint32_t profile_level_id_ = 0;
    std::int64_t next_pts_ = 0;
    std::vector<std::uint8_t> scaled_bgra_;
};

} // namespace

std::unique_ptr<SoftwareH264Encoder> create_x264_h264_encoder(
    std::uint32_t width, std::uint32_t height, std::uint32_t fps,
    std::uint32_t bitrate_bps) {
    return std::make_unique<X264H264Encoder>(width, height, fps, bitrate_bps);
}

} // namespace remoe
