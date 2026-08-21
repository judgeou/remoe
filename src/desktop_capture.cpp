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
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &device_, &feature_level, &context_),
             "D3D11CreateDevice");

    DXGI_OUTPUT_DESC output_desc{};
    check_hr(output_->GetDesc(&output_desc), "IDXGIOutput::GetDesc");
    width_ = static_cast<std::uint32_t>(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
    height_ = static_cast<std::uint32_t>(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
    create_duplication();
}

void DesktopCapture::create_duplication() {
    duplication_.Reset();
    check_hr(output_->DuplicateOutput(device_.Get(), &duplication_), "IDXGIOutput1::DuplicateOutput");
}

bool DesktopCapture::acquire(ID3D11Texture2D* destination, std::chrono::milliseconds timeout) {
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(static_cast<UINT>(timeout.count()), &frame_info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        create_duplication();
        return false;
    }
    check_hr(hr, "AcquireNextFrame");

    struct ReleaseGuard {
        IDXGIOutputDuplication* duplication;
        ~ReleaseGuard() { duplication->ReleaseFrame(); }
    } guard{duplication_.Get()};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    check_hr(resource.As(&source), "Query captured ID3D11Texture2D");
    context_->CopyResource(destination, source.Get());
    return true;
}

} // namespace remoe
