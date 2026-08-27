#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::uint32_t output = 0;
    std::uint32_t timeout_ms = 3000;
    bool list_only = false;
};

struct CaptureResult {
    CaptureResult() = default;
    CaptureResult(bool ok) : api_ok(ok) {}
    CaptureResult(bool ok, bool suspicious)
        : api_ok(ok), suspicious_pixels(suspicious) {}

    bool api_ok = false;
    bool suspicious_pixels = false;
};

struct DxgiOutput {
    std::uint32_t global_index = 0;
    std::uint32_t adapter_index = 0;
    std::uint32_t local_index = 0;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    DXGI_OUTPUT_DESC output_desc{};
};

std::wstring hresult_text(HRESULT hr) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0,
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result;
    if (length != 0 && message) {
        result.assign(message, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
            result.pop_back();
        }
    }
    if (message) LocalFree(message);
    return result;
}

void print_hr(std::wstring_view operation, HRESULT hr) {
    std::wcout << L"    " << operation << L": FAIL HRESULT=0x"
               << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
               << static_cast<std::uint32_t>(hr) << std::dec << std::nouppercase
               << std::setfill(L' ');
    const std::wstring message = hresult_text(hr);
    if (!message.empty()) std::wcout << L" (" << message << L")";
    std::wcout << L'\n';
}

const wchar_t* feature_level_name(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_11_1: return L"11.1";
    case D3D_FEATURE_LEVEL_11_0: return L"11.0";
    case D3D_FEATURE_LEVEL_10_1: return L"10.1";
    case D3D_FEATURE_LEVEL_10_0: return L"10.0";
    case D3D_FEATURE_LEVEL_9_3: return L"9.3";
    case D3D_FEATURE_LEVEL_9_2: return L"9.2";
    case D3D_FEATURE_LEVEL_9_1: return L"9.1";
    default: return L"unknown";
    }
}

std::vector<DxgiOutput> enumerate_dxgi_outputs() {
    ComPtr<IDXGIFactory1> factory;
    const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factory_hr)) {
        print_hr(L"CreateDXGIFactory1", factory_hr);
        return {};
    }

    std::vector<DxgiOutput> outputs;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(adapter_hr)) {
            print_hr(L"IDXGIFactory1::EnumAdapters1", adapter_hr);
            break;
        }

        DXGI_ADAPTER_DESC1 adapter_desc{};
        const HRESULT desc_hr = adapter->GetDesc1(&adapter_desc);
        if (FAILED(desc_hr)) {
            print_hr(L"IDXGIAdapter1::GetDesc1", desc_hr);
            continue;
        }

        std::wcout << L"Adapter " << adapter_index << L": " << adapter_desc.Description
                   << L" vendor=0x" << std::hex << std::setw(4) << std::setfill(L'0')
                   << adapter_desc.VendorId << L" device=0x" << std::setw(4)
                   << adapter_desc.DeviceId << std::dec << std::setfill(L' ')
                   << L" dedicated-video-memory="
                   << (adapter_desc.DedicatedVideoMemory / (1024 * 1024)) << L" MiB"
                   << ((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" [software]" : L"")
                   << L'\n';

        for (UINT local_index = 0;; ++local_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_hr = adapter->EnumOutputs(local_index, &output);
            if (output_hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(output_hr)) {
                print_hr(L"IDXGIAdapter1::EnumOutputs", output_hr);
                break;
            }

            DXGI_OUTPUT_DESC output_desc{};
            const HRESULT output_desc_hr = output->GetDesc(&output_desc);
            if (FAILED(output_desc_hr)) {
                print_hr(L"IDXGIOutput::GetDesc", output_desc_hr);
                continue;
            }

            const LONG width = output_desc.DesktopCoordinates.right -
                               output_desc.DesktopCoordinates.left;
            const LONG height = output_desc.DesktopCoordinates.bottom -
                                output_desc.DesktopCoordinates.top;
            const std::uint32_t global_index = static_cast<std::uint32_t>(outputs.size());
            std::wcout << L"  Output " << global_index << L" (adapter " << adapter_index
                       << L", local " << local_index << L"): " << output_desc.DeviceName
                       << L" rect=(" << output_desc.DesktopCoordinates.left << L','
                       << output_desc.DesktopCoordinates.top << L") " << width << L'x'
                       << height << (output_desc.AttachedToDesktop ? L" attached" : L" detached")
                       << L'\n';

            outputs.push_back(DxgiOutput{
                global_index, adapter_index, local_index, adapter, output,
                adapter_desc, output_desc});
        }
    }
    return outputs;
}

bool save_bgra_bmp(const std::filesystem::path& path, const std::uint8_t* pixels,
                   std::uint32_t width, std::uint32_t height, std::size_t row_pitch,
                   bool source_is_top_down) {
    if (!pixels || width == 0 || height == 0) return false;
    const std::uint64_t row_bytes64 = static_cast<std::uint64_t>(width) * 4;
    const std::uint64_t image_bytes64 = row_bytes64 * height;
    if (image_bytes64 > std::numeric_limits<DWORD>::max()) return false;

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(image_bytes64);
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = static_cast<LONG>(width);
    info_header.biHeight = static_cast<LONG>(height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = static_cast<DWORD>(image_bytes64);

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    file.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));
    const std::size_t row_bytes = static_cast<std::size_t>(row_bytes64);
    for (std::uint32_t destination_row = 0; destination_row < height; ++destination_row) {
        const std::uint32_t source_row = source_is_top_down
            ? height - 1 - destination_row
            : destination_row;
        file.write(reinterpret_cast<const char*>(pixels + source_row * row_pitch), row_bytes);
    }
    return file.good();
}

bool looks_uniform(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
                   std::size_t row_pitch) {
    std::uint8_t minimum = 255;
    std::uint8_t maximum = 0;
    const std::uint32_t step_x = std::max<std::uint32_t>(1, width / 128);
    const std::uint32_t step_y = std::max<std::uint32_t>(1, height / 128);
    for (std::uint32_t y = 0; y < height; y += step_y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * row_pitch;
        for (std::uint32_t x = 0; x < width; x += step_x) {
            const auto* bgra = row + static_cast<std::size_t>(x) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                minimum = std::min(minimum, bgra[channel]);
                maximum = std::max(maximum, bgra[channel]);
            }
        }
    }
    return static_cast<unsigned>(maximum) - minimum <= 2;
}

CaptureResult test_gdi_capture() {
    std::wcout << L"\n[GDI BitBlt]\n";
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    std::wcout << L"    virtual desktop rect=(" << left << L',' << top << L") "
               << width << L'x' << height << L'\n';
    if (width <= 0 || height <= 0) {
        std::wcout << L"    GetSystemMetrics: FAIL invalid virtual desktop dimensions\n";
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (!screen) {
        std::wcout << L"    GetDC(NULL): FAIL Win32 error=" << GetLastError() << L'\n';
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        std::wcout << L"    CreateCompatibleDC: FAIL Win32 error=" << GetLastError() << L'\n';
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        std::wcout << L"    CreateDIBSection: FAIL Win32 error=" << GetLastError() << L'\n';
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ previous = SelectObject(memory, bitmap);
    SetLastError(ERROR_SUCCESS);
    const BOOL copied = BitBlt(memory, 0, 0, width, height, screen, left, top,
                               SRCCOPY | CAPTUREBLT);
    const DWORD bitblt_error = GetLastError();
    GdiFlush();

    bool saved = false;
    bool suspicious = false;
    if (copied) {
        const auto path = std::filesystem::absolute(L"gdi_virtual_desktop.bmp");
        const auto* bytes = static_cast<const std::uint8_t*>(pixels);
        saved = save_bgra_bmp(path, bytes, static_cast<std::uint32_t>(width),
                              static_cast<std::uint32_t>(height),
                              static_cast<std::size_t>(width) * 4, true);
        if (saved) {
            std::wcout << L"    BitBlt: PASS -> " << path.wstring() << L'\n';
            suspicious = looks_uniform(bytes, static_cast<std::uint32_t>(width),
                                       static_cast<std::uint32_t>(height),
                                       static_cast<std::size_t>(width) * 4);
            if (suspicious) {
                std::wcout << L"    WARNING: sampled image is nearly uniform; inspect the BMP for a black/blank frame\n";
            }
        } else {
            std::wcout << L"    BMP write: FAIL\n";
        }
    } else {
        std::wcout << L"    BitBlt: FAIL Win32 error=" << bitblt_error << L'\n';
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return {copied && saved, suspicious};
}

CaptureResult test_dxgi_capture(const DxgiOutput& selected, std::uint32_t timeout_ms) {
    std::wcout << L"\n[DXGI Desktop Duplication output " << selected.global_index << L"]\n";
    std::wcout << L"    adapter: " << selected.adapter_desc.Description << L'\n';

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL feature_level{};
    const HRESULT device_hr = D3D11CreateDevice(
        selected.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, &feature_level, &context);
    if (FAILED(device_hr)) {
        print_hr(L"D3D11CreateDevice on output adapter", device_hr);
        return false;
    }
    std::wcout << L"    D3D11CreateDevice: PASS feature-level="
               << feature_level_name(feature_level) << L'\n';

    ComPtr<ID3D11VideoDevice> video_device;
    const HRESULT video_hr = device.As(&video_device);
    if (SUCCEEDED(video_hr)) {
        std::wcout << L"    Query ID3D11VideoDevice: PASS\n";
    } else {
        print_hr(L"Query ID3D11VideoDevice (remoe GPU scaling would fail)", video_hr);
    }

    ComPtr<IDXGIOutput1> output1;
    const HRESULT output1_hr = selected.output.As(&output1);
    if (FAILED(output1_hr)) {
        print_hr(L"Query IDXGIOutput1", output1_hr);
        return false;
    }
    std::wcout << L"    Query IDXGIOutput1: PASS\n";

    ComPtr<IDXGIOutputDuplication> duplication;
    const HRESULT duplicate_hr = output1->DuplicateOutput(device.Get(), &duplication);
    if (FAILED(duplicate_hr)) {
        print_hr(L"IDXGIOutput1::DuplicateOutput", duplicate_hr);
        return false;
    }
    std::wcout << L"    IDXGIOutput1::DuplicateOutput: PASS\n";

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT acquire_hr = duplication->AcquireNextFrame(timeout_ms, &frame_info, &resource);
    if (FAILED(acquire_hr)) {
        print_hr(L"IDXGIOutputDuplication::AcquireNextFrame", acquire_hr);
        if (acquire_hr == DXGI_ERROR_WAIT_TIMEOUT) {
            std::wcout << L"    Hint: retry with --timeout 10000 while moving a window on the server desktop\n";
        }
        return false;
    }
    struct FrameReleaser {
        IDXGIOutputDuplication* duplication = nullptr;
        ~FrameReleaser() { if (duplication) duplication->ReleaseFrame(); }
    } frame_releaser{duplication.Get()};
    std::wcout << L"    IDXGIOutputDuplication::AcquireNextFrame: PASS\n";

    ComPtr<ID3D11Texture2D> source;
    const HRESULT texture_hr = resource.As(&source);
    if (FAILED(texture_hr)) {
        print_hr(L"Query captured ID3D11Texture2D", texture_hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    std::wcout << L"    captured texture: " << source_desc.Width << L'x'
               << source_desc.Height << L" format=" << source_desc.Format << L'\n';

    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT staging_hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(staging_hr)) {
        print_hr(L"CreateTexture2D staging readback", staging_hr);
        return false;
    }
    context->CopyResource(staging.Get(), source.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_hr)) {
        print_hr(L"Map staging texture", map_hr);
        return false;
    }
    struct Unmapper {
        ID3D11DeviceContext* context = nullptr;
        ID3D11Resource* resource = nullptr;
        ~Unmapper() { if (context && resource) context->Unmap(resource, 0); }
    } unmapper{context.Get(), staging.Get()};

    const auto filename = L"dxgi_output_" + std::to_wstring(selected.global_index) + L".bmp";
    const auto path = std::filesystem::absolute(filename);
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    const bool saved = save_bgra_bmp(path, bytes, source_desc.Width, source_desc.Height,
                                     mapped.RowPitch, true);
    if (!saved) {
        std::wcout << L"    BMP write: FAIL\n";
        return false;
    }
    std::wcout << L"    CPU readback: PASS -> " << path.wstring() << L'\n';
    const bool suspicious = looks_uniform(bytes, source_desc.Width, source_desc.Height,
                                          mapped.RowPitch);
    if (suspicious) {
        std::wcout << L"    WARNING: sampled image is nearly uniform; inspect the BMP for a black/blank frame\n";
    }
    return {true, suspicious};
}

std::optional<std::uint32_t> parse_u32(std::wstring_view value) {
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') return std::nullopt;
        parsed = parsed * 10 + static_cast<unsigned>(character - L'0');
        if (parsed > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
}

void print_usage() {
    std::wcout <<
        L"remoe_capture_probe - GDI and DXGI desktop capture compatibility probe\n\n"
        L"Usage:\n"
        L"  remoe_capture_probe.exe [--output N] [--timeout MS]\n"
        L"  remoe_capture_probe.exe --list\n\n"
        L"Options:\n"
        L"  --output N    Global DXGI output index to test (default 0)\n"
        L"  --timeout MS  AcquireNextFrame timeout, 1-60000 (default 3000)\n"
        L"  --list        List adapters/outputs without capturing\n";
}

std::optional<Options> parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument = argv[index];
        if (argument == L"--help" || argument == L"-h") {
            print_usage();
            return std::nullopt;
        }
        if (argument == L"--list") {
            options.list_only = true;
            continue;
        }
        if ((argument == L"--output" || argument == L"--timeout") && index + 1 < argc) {
            const auto value = parse_u32(argv[++index]);
            if (!value) {
                std::wcerr << L"Invalid numeric value for " << argument << L'\n';
                return std::nullopt;
            }
            if (argument == L"--output") {
                options.output = *value;
            } else {
                if (*value < 1 || *value > 60000) {
                    std::wcerr << L"--timeout must be between 1 and 60000 ms\n";
                    return std::nullopt;
                }
                options.timeout_ms = *value;
            }
            continue;
        }
        std::wcerr << L"Unknown or incomplete option: " << argument << L'\n';
        print_usage();
        return std::nullopt;
    }
    return options;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetProcessDPIAware();
    const auto options = parse_options(argc, argv);
    if (!options) return argc > 1 ? 2 : 0;

    std::wcout << L"remoe capture compatibility probe\n"
               << L"Run this in the same interactive desktop session that remoe_host will use.\n\n"
               << L"[DXGI enumeration]\n";
    const auto outputs = enumerate_dxgi_outputs();
    std::wcout << L"DXGI outputs found: " << outputs.size() << L'\n';
    if (options->list_only) return 0;

    const CaptureResult gdi = test_gdi_capture();
    CaptureResult dxgi;
    if (options->output >= outputs.size()) {
        std::wcout << L"\n[DXGI Desktop Duplication]\n"
                   << L"    FAIL requested output " << options->output
                   << L" does not exist\n";
    } else {
        dxgi = test_dxgi_capture(outputs[options->output], options->timeout_ms);
    }

    const auto result_name = [](const CaptureResult& result) {
        if (!result.api_ok) return L"FAIL";
        return result.suspicious_pixels ? L"SUSPICIOUS (API passed, pixels nearly uniform)"
                                        : L"PASS";
    };
    std::wcout << L"\n[Summary]\n"
               << L"    GDI BitBlt: " << result_name(gdi) << L'\n'
               << L"    DXGI Desktop Duplication + CPU readback: "
               << result_name(dxgi) << L'\n'
               << L"    Compatible capture path: ";
    if (dxgi.api_ok && !dxgi.suspicious_pixels && gdi.api_ok) {
        std::wcout << L"DXGI preferred; GDI fallback available\n";
    } else if (dxgi.api_ok && !dxgi.suspicious_pixels) {
        std::wcout << L"DXGI only\n";
    } else if (gdi.api_ok && !gdi.suspicious_pixels) {
        std::wcout << (dxgi.api_ok
            ? L"GDI preferred; DXGI returned a suspicious frame\n"
            : L"GDI only (current remoe_host DXGI path is incompatible)\n");
    } else {
        std::wcout << L"none in this session\n";
    }
    std::wcout << L"Inspect each generated BMP before accepting a PASS result.\n";
    return (gdi.api_ok || dxgi.api_ok) ? 0 : 1;
}
