#pragma once

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace remoe {

class GdiCapture {
public:
    explicit GdiCapture(std::uint32_t output_index);
    ~GdiCapture();

    GdiCapture(const GdiCapture&) = delete;
    GdiCapture& operator=(const GdiCapture&) = delete;

    bool acquire(std::chrono::milliseconds timeout);

    [[nodiscard]] const std::uint8_t* pixels() const noexcept {
        return static_cast<const std::uint8_t*>(pixels_);
    }
    [[nodiscard]] std::uint32_t stride() const noexcept { return width_ * 4; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::int32_t left() const noexcept { return left_; }
    [[nodiscard]] std::int32_t top() const noexcept { return top_; }
    [[nodiscard]] const std::wstring& device_name() const noexcept { return device_name_; }

private:
    void release_resources() noexcept;
    void draw_cursor();

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int32_t left_ = 0;
    std::int32_t top_ = 0;
    std::wstring device_name_;
    HDC screen_dc_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    void* pixels_ = nullptr;
};

} // namespace remoe
