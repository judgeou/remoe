#include "desktop_capture.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace remoe {
namespace {

void check_hr(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed, HRESULT=0x" +
                                 [] (HRESULT value) {
                                     char text[16]{};
                                     sprintf_s(text, "%08lX", static_cast<unsigned long>(value));
                                     return std::string(text);
                                 }(hr));
    }
}

} // namespace

DesktopCapture::DesktopCapture(std::uint32_t output_index) : output_index_(output_index) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    std::uint32_t remaining = output_index;
    for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT local_output = 0;; ++local_output) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(local_output, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (remaining-- == 0) {
                adapter_ = adapter;
                check_hr(output.As(&output_), "Query IDXGIOutput1");
                break;
            }
        }
        if (output_) break;
    }
    if (!output_) throw std::runtime_error("display output index is out of range");

    D3D_FEATURE_LEVEL feature_level{};
    check_hr(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                               nullptr, 0,
                               D3D11_SDK_VERSION, &device_, &feature_level, &context_),
             "D3D11CreateDevice");
    check_hr(device_.As(&video_device_), "Query ID3D11VideoDevice");
    check_hr(context_.As(&video_context_), "Query ID3D11VideoContext");

    DXGI_OUTPUT_DESC output_desc{};
    check_hr(output_->GetDesc(&output_desc), "IDXGIOutput::GetDesc");
    left_ = output_desc.DesktopCoordinates.left;
    top_ = output_desc.DesktopCoordinates.top;
    width_ = static_cast<std::uint32_t>(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
    height_ = static_cast<std::uint32_t>(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
    create_duplication();
}

void DesktopCapture::use_device(ID3D11Device* device) {
    if (!device) throw std::runtime_error("cannot capture with a null D3D11 device");
    if (device == device_.Get()) return;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    check_hr(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)), "Query IDXGIDevice");
    Microsoft::WRL::ComPtr<IDXGIAdapter> device_adapter;
    check_hr(dxgi_device->GetAdapter(&device_adapter), "IDXGIDevice::GetAdapter");
    DXGI_ADAPTER_DESC device_description{};
    DXGI_ADAPTER_DESC1 capture_description{};
    check_hr(device_adapter->GetDesc(&device_description), "IDXGIAdapter::GetDesc");
    check_hr(adapter_->GetDesc1(&capture_description), "IDXGIAdapter1::GetDesc1");
    if (device_description.AdapterLuid.HighPart != capture_description.AdapterLuid.HighPart ||
        device_description.AdapterLuid.LowPart != capture_description.AdapterLuid.LowPart) {
        throw std::runtime_error("encoder D3D11 device belongs to a different display adapter");
    }

    duplication_.Reset();
    video_processor_.Reset();
    video_enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
    context_.Reset();
    device_ = device;
    device_->GetImmediateContext(&context_);
    if (!context_) throw std::runtime_error("ID3D11Device::GetImmediateContext failed");
    check_hr(device_.As(&video_device_), "Query ID3D11VideoDevice");
    check_hr(context_.As(&video_context_), "Query ID3D11VideoContext");
    scaler_source_width_ = 0;
    scaler_source_height_ = 0;
    scaler_destination_width_ = 0;
    scaler_destination_height_ = 0;
    scaler_source_format_ = DXGI_FORMAT_UNKNOWN;
    scaler_destination_format_ = DXGI_FORMAT_UNKNOWN;
    create_duplication();
}

void DesktopCapture::create_duplication() {
    check_hr(try_create_duplication(), "IDXGIOutput1::DuplicateOutput");
}

HRESULT DesktopCapture::try_create_duplication() {
    duplication_.Reset();
    return output_->DuplicateOutput(device_.Get(), &duplication_);
}

bool DesktopCapture::acquire(ID3D11Texture2D* destination, std::uint32_t content_width,
                             std::uint32_t content_height,
                             std::chrono::milliseconds timeout) {
    if (!duplication_) {
        const HRESULT recreate = try_create_duplication();
        if (recreate == E_ACCESSDENIED || recreate == DXGI_ERROR_UNSUPPORTED ||
            recreate == DXGI_ERROR_SESSION_DISCONNECTED) {
            return false;
        }
        check_hr(recreate, "IDXGIOutput1::DuplicateOutput after desktop switch");
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(static_cast<UINT>(timeout.count()), &frame_info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == E_ACCESSDENIED ||
        hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
        // A UAC prompt switches the visible input desktop to Winsta0\Winlogon.
        // Keep the Host alive and recreate duplication on a later frame. A
        // LocalSystem service worker can recreate against the secure desktop;
        // an ordinary user process resumes after Windows returns to Default.
        duplication_.Reset();
        return false;
    }
    check_hr(hr, "AcquireNextFrame");

    struct ReleaseGuard {
        IDXGIOutputDuplication* duplication;
        ~ReleaseGuard() { duplication->ReleaseFrame(); }
    } guard{duplication_.Get()};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    check_hr(resource.As(&source), "Query captured ID3D11Texture2D");
    D3D11_TEXTURE2D_DESC source_description{};
    D3D11_TEXTURE2D_DESC destination_description{};
    source->GetDesc(&source_description);
    destination->GetDesc(&destination_description);
    if (source_description.Width == content_width &&
        source_description.Height == content_height &&
        destination_description.Width == content_width &&
        destination_description.Height == content_height &&
        source_description.Format == destination_description.Format) {
        context_->CopyResource(destination, source.Get());
    } else {
        scale_texture(source.Get(), destination, source_description.Width, source_description.Height,
                      content_width, content_height, source_description.Format,
                      destination_description.Format);
    }
    return true;
}

void DesktopCapture::scale_texture(ID3D11Texture2D* source, ID3D11Texture2D* destination,
                                   std::uint32_t source_width, std::uint32_t source_height,
                                   std::uint32_t destination_width,
                                   std::uint32_t destination_height,
                                   DXGI_FORMAT source_format,
                                   DXGI_FORMAT destination_format) {
    if (!video_processor_ || source_width != scaler_source_width_ ||
        source_height != scaler_source_height_ ||
        destination_width != scaler_destination_width_ ||
        destination_height != scaler_destination_height_ ||
        source_format != scaler_source_format_ ||
        destination_format != scaler_destination_format_) {
        video_processor_.Reset();
        video_enumerator_.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
        description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        description.InputFrameRate = {60, 1};
        description.InputWidth = source_width;
        description.InputHeight = source_height;
        description.OutputFrameRate = {60, 1};
        description.OutputWidth = destination_width;
        description.OutputHeight = destination_height;
        description.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        check_hr(video_device_->CreateVideoProcessorEnumerator(&description, &video_enumerator_),
                 "CreateVideoProcessorEnumerator(capture scaler)");
        check_hr(video_device_->CreateVideoProcessor(video_enumerator_.Get(), 0, &video_processor_),
                 "CreateVideoProcessor(capture scaler)");
        scaler_source_width_ = source_width;
        scaler_source_height_ = source_height;
        scaler_destination_width_ = destination_width;
        scaler_destination_height_ = destination_height;
        scaler_source_format_ = source_format;
        scaler_destination_format_ = destination_format;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description{};
    input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    check_hr(video_device_->CreateVideoProcessorInputView(source, video_enumerator_.Get(),
                                                           &input_description, &input_view),
             "CreateVideoProcessorInputView(capture scaler)");

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description{};
    output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    check_hr(video_device_->CreateVideoProcessorOutputView(destination, video_enumerator_.Get(),
                                                            &output_description, &output_view),
             "CreateVideoProcessorOutputView(capture scaler)");

    RECT source_rectangle{0, 0, static_cast<LONG>(source_width), static_cast<LONG>(source_height)};
    RECT destination_rectangle{0, 0, static_cast<LONG>(destination_width),
                               static_cast<LONG>(destination_height)};
    video_context_->VideoProcessorSetStreamSourceRect(video_processor_.Get(), 0, TRUE,
                                                       &source_rectangle);
    video_context_->VideoProcessorSetStreamDestRect(video_processor_.Get(), 0, TRUE,
                                                     &destination_rectangle);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    check_hr(video_context_->VideoProcessorBlt(video_processor_.Get(), output_view.Get(), 0, 1,
                                                &stream),
             "VideoProcessorBlt(capture scaler)");
}

} // namespace remoe
