#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>

namespace remoe {

class DesktopCapture {
public:
    explicit DesktopCapture(std::uint32_t output_index);

    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::int32_t left() const noexcept { return left_; }
    [[nodiscard]] std::int32_t top() const noexcept { return top_; }

    // Recreate desktop duplication and video processing on another D3D11 device
    // belonging to the same output adapter (used by oneVPL's internal device).
    void use_device(ID3D11Device* device);

    // Returns true when a new desktop image was acquired, false on timeout.
    bool acquire(ID3D11Texture2D* destination, std::uint32_t content_width,
                 std::uint32_t content_height, std::chrono::milliseconds timeout);

private:
    void create_duplication();
    [[nodiscard]] HRESULT try_create_duplication();
    void scale_texture(ID3D11Texture2D* source, ID3D11Texture2D* destination,
                       std::uint32_t source_width, std::uint32_t source_height,
                       std::uint32_t destination_width, std::uint32_t destination_height,
                       DXGI_FORMAT source_format, DXGI_FORMAT destination_format);

    std::uint32_t output_index_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int32_t left_ = 0;
    std::int32_t top_ = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> video_enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> video_processor_;
    std::uint32_t scaler_source_width_ = 0;
    std::uint32_t scaler_source_height_ = 0;
    std::uint32_t scaler_destination_width_ = 0;
    std::uint32_t scaler_destination_height_ = 0;
    DXGI_FORMAT scaler_source_format_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT scaler_destination_format_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace remoe
