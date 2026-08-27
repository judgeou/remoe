#pragma once

#include "protocol.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <unordered_set>

namespace remoe {

class VideoWindow {
public:
    VideoWindow(std::uint32_t width, std::uint32_t height);
    ~VideoWindow();

    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] const std::atomic_bool* running_flag() const noexcept { return &running_; }
    void stop() noexcept { running_ = false; }
    void request_close() noexcept;
    int message_loop();
    void update_transfer_statistics(double video_mbps, double network_mb_per_second) noexcept;
    void update_frame_age(double age_ms) noexcept;
    void set_input_callback(std::function<bool(const protocol::InputEvent&)> callback);

    // Called by the decoder thread. The input texture remains GPU-resident.
    void present(ID3D11Texture2D* texture, std::uint32_t width, std::uint32_t height);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void create_device_and_swapchain(std::uint32_t width, std::uint32_t height);
    void resize_swapchain(std::uint32_t width, std::uint32_t height);
    void ensure_video_processor(std::uint32_t input_width, std::uint32_t input_height,
                                std::uint32_t output_width, std::uint32_t output_height);
    bool send_mouse_move(LONG x, LONG y);
    void send_mouse_button(protocol::InputType type, bool release);
    void send_keyboard(LPARAM key_data, bool release);
    void release_local_inputs();
    bool send_input(protocol::InputEvent event);

    HWND window_ = nullptr;
    std::atomic_bool running_{true};
    std::atomic<double> frame_age_ms_{0.0};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> video_enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> video_processor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
    std::uint32_t swapchain_width_ = 0;
    std::uint32_t swapchain_height_ = 0;
    std::uint32_t processor_input_width_ = 0;
    std::uint32_t processor_input_height_ = 0;
    std::uint32_t processor_output_width_ = 0;
    std::uint32_t processor_output_height_ = 0;
    std::atomic_uint32_t video_width_{0};
    std::atomic_uint32_t video_height_{0};
    std::function<bool(const protocol::InputEvent&)> input_callback_;
    std::uint32_t input_sequence_ = 0;
    std::unordered_set<std::uint32_t> pressed_keys_;
    std::unordered_set<protocol::InputType> pressed_buttons_;
};

} // namespace remoe
