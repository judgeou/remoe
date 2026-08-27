#include "gdi_capture.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace remoe {
namespace {

struct MonitorDescription {
    RECT rectangle{};
    std::wstring device_name;
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFOEXW information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(monitor, &information)) return TRUE;
    auto& monitors = *reinterpret_cast<std::vector<MonitorDescription>*>(context);
    monitors.push_back({information.rcMonitor, information.szDevice});
    return TRUE;
}

std::runtime_error win32_error(const char* operation, DWORD error = GetLastError()) {
    return std::runtime_error(std::string(operation) + " failed, Win32 error " +
                              std::to_string(error));
}

} // namespace

GdiCapture::GdiCapture(std::uint32_t output_index) {
    SetProcessDPIAware();

    std::vector<MonitorDescription> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                             reinterpret_cast<LPARAM>(&monitors))) {
        throw win32_error("EnumDisplayMonitors");
    }
    if (output_index >= monitors.size()) {
        throw std::runtime_error("GDI display output index is out of range");
    }

    const auto& selected = monitors[output_index];
    left_ = selected.rectangle.left;
    top_ = selected.rectangle.top;
    width_ = static_cast<std::uint32_t>(selected.rectangle.right - selected.rectangle.left);
    height_ = static_cast<std::uint32_t>(selected.rectangle.bottom - selected.rectangle.top);
    device_name_ = selected.device_name;
    if (width_ == 0 || height_ == 0) {
        throw std::runtime_error("selected GDI display has invalid dimensions");
    }

    try {
        screen_dc_ = GetDC(nullptr);
        if (!screen_dc_) throw win32_error("GetDC(NULL)");
        memory_dc_ = CreateCompatibleDC(screen_dc_);
        if (!memory_dc_) throw win32_error("CreateCompatibleDC");

        BITMAPINFO bitmap_information{};
        bitmap_information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_information.bmiHeader.biWidth = static_cast<LONG>(width_);
        bitmap_information.bmiHeader.biHeight = -static_cast<LONG>(height_);
        bitmap_information.bmiHeader.biPlanes = 1;
        bitmap_information.bmiHeader.biBitCount = 32;
        bitmap_information.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(screen_dc_, &bitmap_information, DIB_RGB_COLORS,
                                   &pixels_, nullptr, 0);
        if (!bitmap_ || !pixels_) throw win32_error("CreateDIBSection");
        previous_bitmap_ = SelectObject(memory_dc_, bitmap_);
        if (!previous_bitmap_ || previous_bitmap_ == HGDI_ERROR) {
            throw win32_error("SelectObject(capture bitmap)");
        }
    } catch (...) {
        release_resources();
        throw;
    }
}

GdiCapture::~GdiCapture() {
    release_resources();
}

void GdiCapture::release_resources() noexcept {
    if (memory_dc_ && previous_bitmap_ && previous_bitmap_ != HGDI_ERROR) {
        SelectObject(memory_dc_, previous_bitmap_);
    }
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_dc_) DeleteDC(memory_dc_);
    if (screen_dc_) ReleaseDC(nullptr, screen_dc_);
    previous_bitmap_ = nullptr;
    bitmap_ = nullptr;
    memory_dc_ = nullptr;
    screen_dc_ = nullptr;
    pixels_ = nullptr;
}

bool GdiCapture::acquire(std::chrono::milliseconds) {
    if (!BitBlt(memory_dc_, 0, 0, static_cast<int>(width_), static_cast<int>(height_),
                screen_dc_, left_, top_, SRCCOPY | CAPTUREBLT)) {
        throw win32_error("BitBlt(desktop capture)");
    }
    draw_cursor();
    GdiFlush();
    return true;
}

void GdiCapture::draw_cursor() {
    CURSORINFO cursor_information{};
    cursor_information.cbSize = sizeof(cursor_information);
    if (!GetCursorInfo(&cursor_information) ||
        (cursor_information.flags & CURSOR_SHOWING) == 0 ||
        !cursor_information.hCursor) {
        return;
    }

    ICONINFO icon_information{};
    if (!GetIconInfo(cursor_information.hCursor, &icon_information)) return;
    const int x = cursor_information.ptScreenPos.x - left_ -
                  static_cast<int>(icon_information.xHotspot);
    const int y = cursor_information.ptScreenPos.y - top_ -
                  static_cast<int>(icon_information.yHotspot);
    DrawIconEx(memory_dc_, x, y, cursor_information.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (icon_information.hbmMask) DeleteObject(icon_information.hbmMask);
    if (icon_information.hbmColor) DeleteObject(icon_information.hbmColor);
}

} // namespace remoe
