#include "desktop_capture.h"
#include <windows.h>
#include "video_encoder.h"
#include <deque>
#include <mmsystem.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <string>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
namespace {
constexpr UINT Done = WM_APP + 1;
constexpr UINT Ready = WM_APP + 2;
constexpr UINT Fps = 60;
HWND window{}, button{}, statusText{};
std::thread worker;
std::atomic_bool stop{false};
bool active = false, closing = false;
Clock::time_point started;
std::filesystem::path folder, saved;
std::wstring failure;

void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char code[32];
        sprintf_s(code, " (0x%08lX)", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(operation) + code);
    }
}

UINT primaryOutput() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    UINT index = 0;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o, ++index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            check(output->GetDesc(&desc), "GetDesc");
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(desc.Monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY)) return index;
        }
    }
    throw std::runtime_error("No primary display found");
}





std::filesystem::path findFfmpeg() {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    auto local = std::filesystem::path(executable).parent_path() / L"ffmpeg.exe";
    if (std::filesystem::exists(local)) return local;
    wchar_t found[32768]{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, 32768, found, nullptr)) return found;
    throw std::runtime_error("FFmpeg not found. Put ffmpeg.exe beside remoe_recorder.exe or add it to PATH.");
}

void remux(const std::filesystem::path& ffmpeg, const std::filesystem::path& ivf, const std::filesystem::path& mp4) {
    std::wstring command = L"\"" + ffmpeg.wstring() + L"\" -hide_banner -loglevel error -nostdin -n -i \"" +
        ivf.wstring() + L"\" -map 0:v:0 -c:v copy -an -movflags +faststart \"" + mp4.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) throw std::runtime_error("Cannot start FFmpeg; original IVF recording preserved");
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    if (code != 0) throw std::runtime_error("MP4 remux failed; original IVF recording preserved beside the MP4");
}

void record(const std::filesystem::path& path, double seconds) {
    const auto ffmpeg = findFfmpeg();
    auto ivf = path; ivf.replace_extension(L".ivf");
    if (std::filesystem::exists(path) || std::filesystem::exists(ivf)) throw std::runtime_error("Output already exists");
    remoe::DesktopCapture capture(primaryOutput());
    const UINT width = (capture.width() + 1) & ~1u, height = (capture.height() + 1) & ~1u;
    auto encoder = remoe::create_preferred_av1_encoder(capture.device(), width, height, Fps, 0,
        remoe::protocol::VideoRateControl::FixedQuality, 30);
    capture.use_device(encoder->device());
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET; desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    ComPtr<ID3D11Texture2D> cached, composed;
    check(capture.device()->CreateTexture2D(&desc, nullptr, &cached), "Create capture texture");
    check(capture.device()->CreateTexture2D(&desc, nullptr, &composed), "Create cursor texture");
    ComPtr<IDXGISurface1> surface;
    check(composed.As(&surface), "Cursor surface");
    std::ofstream output(ivf, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    auto little = [&](UINT64 value, int count) {
        for (int i = 0; i < count; ++i) { output.put(static_cast<char>(value & 255)); value >>= 8; }
    };
    output.write("DKIF", 4); little(0, 2); little(32, 2); output.write("AV01", 4);
    little(width, 2); little(height, 2); little(Fps, 4); little(1, 4); little(0, 4); little(0, 4);
    std::deque<UINT64> pending;
    UINT64 frames = 0, ticks = 0, repeats = 0;
    auto writePackets = [&](const std::vector<remoe::EncodedVideoFrame>& packets) {
        for (const auto& packet : packets) {
            if (pending.empty()) throw std::runtime_error("Unexpected AV1 frame without timestamp");
            little(packet.data.size(), 4); little(pending.front(), 8); pending.pop_front();
            output.write(reinterpret_cast<const char*>(packet.data.data()), packet.data.size()); ++frames;
        }
    };
    timeBeginPeriod(1);
    struct Timer { ~Timer() { timeEndPeriod(1); } } timer;
    bool haveFrame = false;
    auto begin = Clock::now();
    const auto waiting = begin;
    if (window) PostMessageW(window, Ready, 0, 0);
    while (!stop.load()) {
        if (!haveFrame) {
            haveFrame = capture.acquire(cached.Get(), width, height, std::chrono::milliseconds(10));
            if (!haveFrame) {
                if (Clock::now() - waiting > std::chrono::seconds(10)) throw std::runtime_error("No desktop frame received");
                continue;
            }
            begin = Clock::now();
        } else if (!capture.acquire(cached.Get(), width, height, std::chrono::milliseconds(0))) ++repeats;
        // Cached image is encoded again for idle desktop frames: output stays CFR 60.
        capture.context()->CopyResource(composed.Get(), cached.Get());
        CURSORINFO cursor{sizeof(cursor)};
        if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING)) {
            HDC dc{};
            check(surface->GetDC(FALSE, &dc), "Get cursor DC");
            ICONINFO icon{};
            if (GetIconInfo(cursor.hCursor, &icon)) {
                DrawIconEx(dc, cursor.ptScreenPos.x - capture.left() - static_cast<int>(icon.xHotspot),
                    cursor.ptScreenPos.y - capture.top() - static_cast<int>(icon.yHotspot), cursor.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                if (icon.hbmMask) DeleteObject(icon.hbmMask);
                if (icon.hbmColor) DeleteObject(icon.hbmColor);
            }
            check(surface->ReleaseDC(nullptr), "Release cursor DC");
        }
        capture.copy_frame(composed.Get(), encoder->input_texture());
        pending.push_back(ticks);
        writePackets(encoder->encode(ticks % (Fps * 2) == 0));
        ++ticks;
        auto next = begin + std::chrono::nanoseconds(ticks * 1000000000 / Fps);
        std::this_thread::sleep_until(next);
        if (seconds > 0 && ticks >= static_cast<UINT64>(seconds * Fps)) break;
    }
    writePackets(encoder->drain());
    if (!pending.empty()) throw std::runtime_error("Encoder did not drain all frames; IVF preserved");
    output.seekp(24); little(frames, 4); output.close();
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    std::ofstream log(path.wstring() + L".log");
    log << "encoder=" << encoder->name() << "\ncodec=AV1\nrate_control=FixedQuality\nquality=30\nfps=60\nwidth="
        << width << "\nheight=" << height << "\nframes=" << frames << "\ncapture_repeats=" << repeats
        << "\nelapsed_seconds=" << elapsed << "\nprocessing_fps=" << (elapsed > 0 ? frames / elapsed : 0) << '\n';
    if (!frames) throw std::runtime_error("Recording stopped before the first frame");
    encoder.reset();
    remux(ffmpeg, ivf, path);
    std::filesystem::remove(ivf);
}

void toggle() {
    if (active) {
        stop = true;
        EnableWindow(button, FALSE);
        SetWindowTextW(statusText, L"正在保存 MP4，请稍候…");
        return;
    }
    SYSTEMTIME time{}; GetLocalTime(&time);
    wchar_t name[100];
    swprintf_s(name, L"录屏_%04u%02u%02u_%02u%02u%02u_%03u.mp4", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    saved = folder / name;
    failure.clear(); stop = false; active = true; started = Clock::now();
    SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
    SetWindowTextW(button, L"停止并保存");
    SetWindowTextW(statusText, L"正在初始化录制…");
    worker = std::thread([] {
        try { record(saved, 0); }
        catch (const std::exception& e) { const std::string s = e.what(); failure.assign(s.begin(), s.end()); }
        PostMessageW(window, Done, 0, 0);
    });
}

LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wp) == 1 && (!active || !stop.load())) toggle();
        if (LOWORD(wp) == 2) ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    case WM_HOTKEY: if (!active || !stop.load()) toggle(); return 0;
    case Ready: started = Clock::now(); SetTimer(hwnd, 1, 250, nullptr); return 0;
    case WM_TIMER:
        if (active && !stop.load()) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started).count();
            wchar_t text[100]; swprintf_s(text, L"● 正在录制  %02lld:%02lld   ·   无声音", seconds / 60, seconds % 60);
            SetWindowTextW(statusText, text);
        }
        return 0;
    case Done:
        worker.join(); active = false; KillTimer(hwnd, 1);
        SetWindowDisplayAffinity(hwnd, WDA_NONE);
        EnableWindow(button, TRUE); SetWindowTextW(button, L"开始录制");
        if (failure.empty()) SetWindowTextW(statusText, (L"已保存：" + saved.filename().wstring()).c_str());
        else { SetWindowTextW(statusText, L"录制未完成，请查看错误信息。"); MessageBoxW(hwnd, failure.c_str(), L"录制失败", MB_OK | MB_ICONERROR); }
        if (closing) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        if (active) { closing = true; if (!stop.load()) toggle(); }
        else DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: UnregisterHotKey(hwnd, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        struct Args { LPWSTR* p; ~Args() { LocalFree(p); } } args{argv};
        if (argc == 4 && std::wstring(argv[1]) == L"--record-seconds") {
            const double seconds = std::stod(argv[2]);
            if (!(seconds > 0 && seconds <= 86400)) throw std::runtime_error("Invalid duration");
            record(std::filesystem::absolute(argv[3]), seconds);
            return 0;
        }
        PWSTR videos{};
        check(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos), "Videos folder");
        folder = std::filesystem::path(videos) / L"remoe 录屏";
        CoTaskMemFree(videos);
        std::filesystem::create_directories(folder);
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = instance;
        wc.lpszClassName = L"RemoeRecorder"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClass failed");
        const UINT dpi = GetDpiForSystem();
        auto scale = [dpi](int x) { return MulDiv(x, dpi, 96); };
        RECT rect{0, 0, scale(480), scale(200)};
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
        window = CreateWindowW(wc.lpszClassName, L"remoe 全屏录制", style, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("CreateWindow failed");
        HFONT font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD extra, int x, int y, int w, int h, int id) {
            HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | extra, scale(x), scale(y), scale(w), scale(h), window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return c;
        };
        control(L"STATIC", L"主屏幕全屏 · 60 帧 · AV1 · 固定质量 30 · MP4", 0, 20, 18, 440, 26, 0);
        statusText = control(L"STATIC", L"准备就绪，无需设置。", 0, 20, 54, 440, 46, 0);
        button = control(L"BUTTON", L"开始录制", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, 105, 210, 42, 1);
        control(L"BUTTON", L"打开保存文件夹", WS_TABSTOP, 246, 105, 214, 42, 2);
        const wchar_t* shortcut = L"快捷键被占用，请使用按钮开始 / 停止。";
        if (RegisterHotKey(window, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R'))
            shortcut = L"Ctrl + Alt + R 开始 / 停止；关闭窗口也会保存。";
        else if (RegisterHotKey(window, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F9))
            shortcut = L"Ctrl + Shift + F9 开始 / 停止；关闭窗口也会保存。";
        control(L"STATIC", shortcut, 0, 20, 163, 440, 26, 0);
        ShowWindow(window, show);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        DeleteObject(font);
        return 0;
    } catch (const std::exception& e) {
        std::ofstream("remoe-recorder-error.log") << e.what();
        // Automated smoke mode reports failure by exit code and log, without a modal dialog.
        if (wcsstr(GetCommandLineW(), L"--record-seconds") == nullptr) MessageBoxA(nullptr, e.what(), "remoe recorder", MB_OK | MB_ICONERROR);
        return 1;
    }
}

