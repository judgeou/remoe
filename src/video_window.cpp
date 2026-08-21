#include "video_window.h"

#include <d3d10.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace remoe {
namespace {

void check_hr(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char code[16]{};
        sprintf_s(code, "%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(operation) + " failed, HRESULT=0x" + code);
    }
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> find_intel_adapter(IDXGIFactory2* factory) {
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 description{};
        check_hr(adapter->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
        if (description.VendorId == 0x8086 &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            return adapter;
        }
    }
    throw std::runtime_error("no Intel graphics adapter was found");
}

} // namespace

VideoWindow::VideoWindow(std::uint32_t width, std::uint32_t height) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = L"RemoeClientWindow";
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("RegisterClassExW failed");
    }

    RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    window_ = CreateWindowExW(0, window_class.lpszClassName, L"remoe client",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                              nullptr, nullptr, instance, this);
    if (!window_) throw std::runtime_error("CreateWindowExW failed");

    create_device_and_swapchain(width, height);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
}

VideoWindow::~VideoWindow() {
    running_ = false;
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
}

void VideoWindow::create_device_and_swapchain(std::uint32_t width, std::uint32_t height) {
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    const auto adapter = find_intel_adapter(factory.Get());

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    check_hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                               levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                               &device_, &selected, &context_),
             "D3D11CreateDevice(Intel)");

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    check_hr(device_.As(&video_device_), "Query ID3D11VideoDevice");
    check_hr(context_.As(&video_context_), "Query ID3D11VideoContext");

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = (std::max)(width, 1u);
    description.Height = (std::max)(height, 1u);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Scaling = DXGI_SCALING_STRETCH;
    check_hr(factory->CreateSwapChainForHwnd(device_.Get(), window_, &description, nullptr,
                                              nullptr, &swapchain_),
             "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    swapchain_width_ = description.Width;
    swapchain_height_ = description.Height;

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
    if (SUCCEEDED(device_.As(&dxgi_device))) dxgi_device->SetMaximumFrameLatency(1);
}

void VideoWindow::resize_swapchain(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 ||
        (width == swapchain_width_ && height == swapchain_height_)) return;
    output_view_.Reset();
    video_processor_.Reset();
    video_enumerator_.Reset();
    context_->Flush();
    check_hr(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0),
             "IDXGISwapChain::ResizeBuffers");
    swapchain_width_ = width;
    swapchain_height_ = height;
}

void VideoWindow::ensure_video_processor(std::uint32_t input_width, std::uint32_t input_height,
                                         std::uint32_t output_width, std::uint32_t output_height) {
    if (video_processor_ && input_width == processor_input_width_ &&
        input_height == processor_input_height_ && output_width == processor_output_width_ &&
        output_height == processor_output_height_) return;

    output_view_.Reset();
    video_processor_.Reset();
    video_enumerator_.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
    description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    description.InputFrameRate = {60, 1};
    description.InputWidth = input_width;
    description.InputHeight = input_height;
    description.OutputFrameRate = {60, 1};
    description.OutputWidth = output_width;
    description.OutputHeight = output_height;
    description.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check_hr(video_device_->CreateVideoProcessorEnumerator(&description, &video_enumerator_),
             "CreateVideoProcessorEnumerator");
    check_hr(video_device_->CreateVideoProcessor(video_enumerator_.Get(), 0, &video_processor_),
             "CreateVideoProcessor");

    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    check_hr(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)), "IDXGISwapChain::GetBuffer");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description{};
    output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    check_hr(video_device_->CreateVideoProcessorOutputView(back_buffer.Get(), video_enumerator_.Get(),
                                                            &output_description, &output_view_),
             "CreateVideoProcessorOutputView");
    processor_input_width_ = input_width;
    processor_input_height_ = input_height;
    processor_output_width_ = output_width;
    processor_output_height_ = output_height;
}

void VideoWindow::present(ID3D11Texture2D* texture, std::uint32_t width, std::uint32_t height) {
    if (!running_ || !texture) return;
    RECT client{};
    if (!GetClientRect(window_, &client)) return;
    const auto output_width = static_cast<std::uint32_t>((std::max)(client.right - client.left, 0L));
    const auto output_height = static_cast<std::uint32_t>((std::max)(client.bottom - client.top, 0L));
    if (output_width == 0 || output_height == 0) return;

    resize_swapchain(output_width, output_height);
    ensure_video_processor(width, height, output_width, output_height);

    D3D11_TEXTURE2D_DESC texture_description{};
    texture->GetDesc(&texture_description);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description{};
    input_description.FourCC = 0;
    input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_description.Texture2D.MipSlice = 0;
    input_description.Texture2D.ArraySlice = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    check_hr(video_device_->CreateVideoProcessorInputView(texture, video_enumerator_.Get(),
                                                           &input_description, &input_view),
             "CreateVideoProcessorInputView");

    const double input_aspect = static_cast<double>(width) / height;
    const double output_aspect = static_cast<double>(output_width) / output_height;
    RECT destination{0, 0, static_cast<LONG>(output_width), static_cast<LONG>(output_height)};
    if (input_aspect > output_aspect) {
        const LONG fitted_height = static_cast<LONG>(output_width / input_aspect);
        destination.top = (static_cast<LONG>(output_height) - fitted_height) / 2;
        destination.bottom = destination.top + fitted_height;
    } else {
        const LONG fitted_width = static_cast<LONG>(output_height * input_aspect);
        destination.left = (static_cast<LONG>(output_width) - fitted_width) / 2;
        destination.right = destination.left + fitted_width;
    }
    RECT source{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    video_context_->VideoProcessorSetStreamSourceRect(video_processor_.Get(), 0, TRUE, &source);
    video_context_->VideoProcessorSetStreamDestRect(video_processor_.Get(), 0, TRUE, &destination);
    D3D11_VIDEO_COLOR background{};
    video_context_->VideoProcessorSetOutputBackgroundColor(video_processor_.Get(), FALSE,
                                                            &background);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    check_hr(video_context_->VideoProcessorBlt(video_processor_.Get(), output_view_.Get(), 0, 1,
                                                &stream),
             "VideoProcessorBlt");
    // VSync avoids needless presentation work and is intentionally favored for low power.
    check_hr(swapchain_->Present(1, 0), "IDXGISwapChain::Present");
}

int VideoWindow::message_loop() {
    MSG message{};
    while (running_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    running_ = false;
    return static_cast<int>(message.wParam);
}

void VideoWindow::request_close() noexcept {
    running_ = false;
    if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK VideoWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    VideoWindow* self = reinterpret_cast<VideoWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<VideoWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_CLOSE) {
        if (self) self->running_ = false;
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        if (self) self->running_ = false;
        PostQuitMessage(0);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace remoe
