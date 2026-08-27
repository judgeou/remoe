#if defined(REMOE_X264_HOST)
#include "gdi_capture.h"
#include "x264_encoder.h"
#else
#include "desktop_capture.h"
#include "video_encoder.h"
#endif
#include "clipboard.h"
#include "encoded_video_frame.h"
#include "host_identity.h"
#include "protocol.h"
#include "webrtc_websocket_signaling.h"

#include <Windows.h>
#include <netfw.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
std::atomic_bool g_running{true};
constexpr wchar_t kFirewallRuleName[] = L"remoe host WebRTC UDP";
constexpr long kFirewallProfiles = NET_FW_PROFILE2_DOMAIN |
                                   NET_FW_PROFILE2_PRIVATE |
                                   NET_FW_PROFILE2_PUBLIC;

bool send_clipboard_text(remoe::WebRtcTransport& transport, std::string_view text,
                         std::uint32_t sequence) {
    auto message = remoe::make_clipboard_message(text, sequence);
    return transport.send_binary(message);
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

struct Options {
    std::uint32_t output = 0;
    // Zero means the operator did not configure a server-side limit.
    std::uint32_t max_fps = 0;
    std::uint32_t max_bitrate_mbps = 0;
    std::string signaling_url;
    bool repair = false;
    bool legacy_invite = false;
    bool check_encoder = false;
};

struct StreamSettings {
    std::uint32_t fps = 60;
    std::uint32_t bitrate_bps = 20'000'000;
    std::uint32_t scale_percent = 100;
};

bool is_process_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw std::runtime_error("OpenProcessToken failed, Win32 error " +
                                 std::to_string(GetLastError()));
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!ok) {
        throw std::runtime_error("GetTokenInformation failed, Win32 error " +
                                 std::to_string(error));
    }
    return elevation.TokenIsElevated != 0;
}

std::wstring current_executable_path() {
    std::vector<wchar_t> executable(32768);
    const DWORD size = GetModuleFileNameW(nullptr, executable.data(),
                                         static_cast<DWORD>(executable.size()));
    if (size == 0 || size == executable.size()) {
        throw std::runtime_error("GetModuleFileNameW failed, Win32 error " +
                                 std::to_string(GetLastError()));
    }
    return std::wstring(executable.data(), size);
}

std::wstring quote_windows_argument(std::wstring_view argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted(1, L'\"');
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

// Returns true in the original process after it successfully launches an elevated child.
bool relaunch_as_admin_if_requested() {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) {
        throw std::runtime_error("CommandLineToArgvW failed, Win32 error " +
                                 std::to_string(GetLastError()));
    }

    bool requested = false;
    std::wstring parameters;
    for (int i = 1; i < argument_count; ++i) {
        if (std::wstring_view(arguments[i]) == L"--admin") {
            requested = true;
            continue;
        }
        if (!parameters.empty()) parameters.push_back(L' ');
        parameters += quote_windows_argument(arguments[i]);
    }
    LocalFree(arguments);

    if (!requested || is_process_elevated()) return false;

    const std::wstring executable = current_executable_path();

    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            throw std::runtime_error("administrator request was cancelled");
        }
        throw std::runtime_error("ShellExecuteExW(runas) failed, Win32 error " +
                                 std::to_string(error));
    }
    if (execute.hProcess) CloseHandle(execute.hProcess);
    return true;
}

class ComInitialization {
public:
    ComInitialization() {
        status_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(status_) && status_ != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("CoInitializeEx failed, HRESULT " +
                                     std::to_string(static_cast<unsigned long>(status_)));
        }
    }
    ~ComInitialization() {
        if (SUCCEEDED(status_)) CoUninitialize();
    }

private:
    HRESULT status_ = E_FAIL;
};

class ScopedBstr {
public:
    ScopedBstr() = default;
    explicit ScopedBstr(const wchar_t* value) : value_(SysAllocString(value)) {
        if (!value_) throw std::bad_alloc();
    }
    ~ScopedBstr() { SysFreeString(value_); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    [[nodiscard]] BSTR get() const noexcept { return value_; }
    [[nodiscard]] BSTR* put() noexcept {
        SysFreeString(value_);
        value_ = nullptr;
        return &value_;
    }

private:
    BSTR value_ = nullptr;
};

Microsoft::WRL::ComPtr<INetFwPolicy2> firewall_policy() {
    Microsoft::WRL::ComPtr<INetFwPolicy2> policy;
    const HRESULT status = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&policy));
    if (FAILED(status)) {
        throw std::runtime_error("Unable to access Windows Firewall policy, HRESULT " +
                                 std::to_string(static_cast<unsigned long>(status)));
    }
    return policy;
}

bool firewall_rule_is_current() {
    ComInitialization com;
    const auto policy = firewall_policy();
    Microsoft::WRL::ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules))) return false;

    const ScopedBstr name(kFirewallRuleName);
    Microsoft::WRL::ComPtr<INetFwRule> rule;
    if (FAILED(rules->Item(name.get(), rule.GetAddressOf())) || !rule) return false;

    ScopedBstr application;
    NET_FW_RULE_DIRECTION direction{};
    NET_FW_ACTION action{};
    long protocol = 0;
    long profiles = 0;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    const bool valid =
        SUCCEEDED(rule->get_ApplicationName(application.put())) &&
        SUCCEEDED(rule->get_Direction(&direction)) &&
        SUCCEEDED(rule->get_Action(&action)) &&
        SUCCEEDED(rule->get_Protocol(&protocol)) &&
        SUCCEEDED(rule->get_Profiles(&profiles)) &&
        SUCCEEDED(rule->get_Enabled(&enabled));
    const std::wstring executable = current_executable_path();
    return valid && application.get() &&
        _wcsicmp(application.get(), executable.c_str()) == 0 &&
        direction == NET_FW_RULE_DIR_IN && action == NET_FW_ACTION_ALLOW &&
        protocol == NET_FW_IP_PROTOCOL_UDP &&
        (profiles & kFirewallProfiles) == kFirewallProfiles &&
        enabled == VARIANT_TRUE;
}

int install_firewall_rule() {
    try {
        if (!is_process_elevated()) {
            throw std::runtime_error(
                "firewall rule installation requires administrator privileges");
        }
        ComInitialization com;
        const auto policy = firewall_policy();
        Microsoft::WRL::ComPtr<INetFwRules> rules;
        HRESULT status = policy->get_Rules(&rules);
        if (FAILED(status)) {
            throw std::runtime_error("INetFwPolicy2::get_Rules failed, HRESULT " +
                                     std::to_string(static_cast<unsigned long>(status)));
        }

        const ScopedBstr name(kFirewallRuleName);
        // Replace a disabled or stale rule left after moving the executable.
        (void)rules->Remove(name.get());

        Microsoft::WRL::ComPtr<INetFwRule> rule;
        status = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&rule));
        if (FAILED(status)) {
            throw std::runtime_error("Unable to create Windows Firewall rule, HRESULT " +
                                     std::to_string(static_cast<unsigned long>(status)));
        }

        const std::wstring executable = current_executable_path();
        const ScopedBstr application(executable.c_str());
        const ScopedBstr description(
            L"Allows inbound WebRTC ICE/DTLS/SCTP UDP traffic for remoe host.");
        status = rule->put_Name(name.get());
        if (SUCCEEDED(status)) status = rule->put_Description(description.get());
        if (SUCCEEDED(status)) status = rule->put_ApplicationName(application.get());
        if (SUCCEEDED(status)) status = rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP);
        if (SUCCEEDED(status)) status = rule->put_Direction(NET_FW_RULE_DIR_IN);
        if (SUCCEEDED(status)) status = rule->put_Action(NET_FW_ACTION_ALLOW);
        if (SUCCEEDED(status)) status = rule->put_Profiles(kFirewallProfiles);
        if (SUCCEEDED(status)) status = rule->put_EdgeTraversal(VARIANT_TRUE);
        if (SUCCEEDED(status)) status = rule->put_Enabled(VARIANT_TRUE);
        if (SUCCEEDED(status)) status = rules->Add(rule.Get());
        if (FAILED(status)) {
            throw std::runtime_error("Adding Windows Firewall rule failed, HRESULT " +
                                     std::to_string(static_cast<unsigned long>(status)));
        }
        std::cout << "Windows Firewall rule installed for remoe_host UDP\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Firewall setup error: " << error.what() << '\n';
        return 1;
    }
}

void ensure_firewall_rule() {
    if (firewall_rule_is_current()) return;

    std::cout << "WebRTC UDP firewall rule is missing; requesting administrator permission...\n"
              << std::flush;
    if (is_process_elevated()) {
        if (install_firewall_rule() != 0 || !firewall_rule_is_current()) {
            throw std::runtime_error("Windows Firewall rule installation failed");
        }
        return;
    }
    const std::wstring executable = current_executable_path();
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = L"--install-firewall-rule";
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            throw std::runtime_error("Windows Firewall permission was cancelled");
        }
        throw std::runtime_error("Unable to launch firewall setup, Win32 error " +
                                 std::to_string(error));
    }
    if (!execute.hProcess) {
        throw std::runtime_error("Windows Firewall setup process did not start");
    }
    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exit_code = 1;
    const BOOL got_exit_code = GetExitCodeProcess(execute.hProcess, &exit_code);
    CloseHandle(execute.hProcess);
    if (!got_exit_code || exit_code != 0) {
        throw std::runtime_error("Windows Firewall rule installation failed");
    }
    if (!firewall_rule_is_current()) {
        throw std::runtime_error("Windows Firewall rule was not installed correctly");
    }
    std::cout << "WebRTC UDP firewall rule is ready\n";
}

std::uint32_t parse_u32(std::string_view text, std::string_view name, std::uint32_t min,
                        std::uint32_t max) {
    std::size_t used = 0;
    unsigned long value = 0;
    try {
        value = std::stoul(std::string(text), &used, 10);
    } catch (...) {
        throw std::runtime_error("invalid value for " + std::string(name));
    }
    if (used != text.size() || value < min || value > max) {
        throw std::runtime_error("value for " + std::string(name) + " is out of range");
    }
    return static_cast<std::uint32_t>(value);
}

void print_help() {
#if defined(REMOE_X264_HOST)
    constexpr const char* description =
        "remoe_host_x264 - Windows GDI software H.264 streaming host\n\n";
    constexpr const char* executable = "remoe_host_x264";
#else
    constexpr const char* description =
        "remoe_host - Windows desktop hardware AV1 streaming host\n\n";
    constexpr const char* executable = "remoe_host";
#endif
    std::cout <<
        description <<
        "Usage: " << executable << " [options]\n"
        "  --output <index>   Desktop output index (default: 0)\n"
        "  --max-fps <1-240>  Optional maximum client frame rate (default: unlimited)\n"
        "  --max-bitrate <Mbps> Optional maximum client bitrate (default: unlimited)\n"
        "  --signal-url <ws(s)://...> WebSocket signaling URL (required)\n"
        "  --repair           Rebind this Host to an account and rotate its credential\n"
        "  --legacy-invite    Print an anonymous invite for the native client\n"
        "  --check-encoder    Encode one test frame and exit (no signaling required)\n"
        "  --fps/--bitrate    Compatibility aliases for the two limits above\n"
        "  --admin            Relaunch with administrator privileges\n"
        "  --help             Show this help\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        }
        if (arg == "--admin") continue; // Consumed by relaunch_as_admin_if_requested().
        if (arg == "--repair") {
            options.repair = true;
            continue;
        }
        if (arg == "--legacy-invite") {
            options.legacy_invite = true;
            continue;
        }
        if (arg == "--check-encoder") {
            options.check_encoder = true;
            continue;
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value after " + std::string(arg));
        const std::string_view value(argv[++i]);
        if (arg == "--output") options.output = parse_u32(value, arg, 0, 63);
        else if (arg == "--max-fps" || arg == "--fps") {
            options.max_fps = parse_u32(value, arg, 1, 240);
        } else if (arg == "--max-bitrate" || arg == "--bitrate") {
            options.max_bitrate_mbps = parse_u32(value, arg, 1, 1000);
        } else if (arg == "--signal-url") options.signaling_url = value;
        else throw std::runtime_error("unknown option: " + std::string(arg));
    }
    if (options.signaling_url.empty() && !options.check_encoder) {
        throw std::runtime_error("--signal-url is required");
    }
    if (options.repair && options.legacy_invite) {
        throw std::runtime_error("--repair cannot be combined with --legacy-invite");
    }
    return options;
}

enum class VideoSendResult { Sent, Dropped, Failed };

VideoSendResult send_packet(remoe::WebRtcTransport& transport,
                            const remoe::EncodedVideoFrame& packet,
                            std::uint64_t frame_number, std::uint64_t timestamp_us) {
    const std::span<const std::uint8_t> encoded_video(packet.data);
    if (encoded_video.empty() ||
        encoded_video.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return VideoSendResult::Failed;
    }
    constexpr std::size_t max_buffered_video = 4 * 1024 * 1024;
    if (transport.video_buffered_amount() > max_buffered_video) {
        return VideoSendResult::Dropped;
    }

    remoe::protocol::VideoChunkHeader header;
    header.frame_size = static_cast<std::uint32_t>(encoded_video.size());
    header.frame_number = frame_number;
    header.timestamp_us = timestamp_us;
    if (packet.key_frame) header.flags |= remoe::protocol::kFrameKey;
    std::vector<std::uint8_t> message(sizeof(header) + remoe::protocol::kVideoChunkPayloadSize);
    for (std::size_t offset = 0; offset < encoded_video.size();
         offset += remoe::protocol::kVideoChunkPayloadSize) {
        const std::size_t chunk_size = (std::min)(
            remoe::protocol::kVideoChunkPayloadSize, encoded_video.size() - offset);
        header.chunk_offset = static_cast<std::uint32_t>(offset);
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), encoded_video.data() + offset,
                    chunk_size);
        if (!transport.send_video_binary(
                std::span<const std::uint8_t>(message.data(), sizeof(header) + chunk_size))) {
            return VideoSendResult::Failed;
        }
    }
    return VideoSendResult::Sent;
}

bool validate_client_settings(const remoe::protocol::ClientConfig& request,
                              const Options& options, StreamSettings& settings) {
    if (request.magic != remoe::protocol::kClientConfigMagic ||
        request.version != remoe::protocol::kVersion ||
        request.header_size != sizeof(request) || request.fps_den != 1 ||
        (request.flags & ~remoe::protocol::kClientClipboardText) != 0 ||
        request.fps_num == 0 || request.fps_num > 240 ||
        request.scale_percent < 10 || request.scale_percent > 100 ||
        (options.max_fps != 0 && request.fps_num > options.max_fps) ||
        request.bitrate_bps < 1'000'000u ||
        request.bitrate_bps > 1'000'000'000u ||
#if defined(REMOE_X264_HOST)
        request.bitrate_bps > 50'000'000u ||
#endif
        (options.max_bitrate_mbps != 0 &&
         request.bitrate_bps > options.max_bitrate_mbps * 1'000'000u)) {
        return false;
    }
    settings.fps = request.fps_num;
    settings.bitrate_bps = request.bitrate_bps;
    settings.scale_percent = request.scale_percent;
    return true;
}

std::uint32_t scaled_dimension(std::uint32_t source, std::uint32_t percent) {
    std::uint32_t scaled = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(source) * percent / 100);
    scaled = (std::max)(scaled, 2u);
    return scaled & ~1u;
}

DWORD mouse_button_flag(remoe::protocol::InputType type, bool release) {
    using remoe::protocol::InputType;
    switch (type) {
    case InputType::MouseLeft: return release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
    case InputType::MouseRight: return release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
    case InputType::MouseMiddle: return release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
    case InputType::MouseX1:
    case InputType::MouseX2: return release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
    default: return 0;
    }
}

bool inject_input_event(const remoe::protocol::InputEvent& event,
                        std::int32_t output_left, std::int32_t output_top,
                        std::uint32_t output_width, std::uint32_t output_height,
                        std::unordered_set<std::uint32_t>& pressed_keys,
                        std::unordered_set<remoe::protocol::InputType>& pressed_buttons,
                        bool& injection_warning_shown) {
    using remoe::protocol::InputType;
    INPUT input{};
    const bool release = (event.flags & remoe::protocol::kInputRelease) != 0;
    if (event.type == InputType::MouseMove) {
        if (event.flags != 0 || event.value1 < 0 || event.value1 > 65535 ||
            event.value2 < 0 || event.value2 > 65535) return false;
        const std::int64_t desktop_x = output_left +
            static_cast<std::int64_t>(event.value1) * (output_width - 1) / 65535;
        const std::int64_t desktop_y = output_top +
            static_cast<std::int64_t>(event.value2) * (output_height - 1) / 65535;
        const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (virtual_width <= 1 || virtual_height <= 1) return false;
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>((desktop_x - virtual_left) * 65535 / (virtual_width - 1));
        input.mi.dy = static_cast<LONG>((desktop_y - virtual_top) * 65535 / (virtual_height - 1));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    } else if (event.type >= InputType::MouseLeft && event.type <= InputType::MouseX2) {
        if ((event.flags & ~remoe::protocol::kInputRelease) != 0) return false;
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = mouse_button_flag(event.type, release);
        if (event.type == InputType::MouseX1) input.mi.mouseData = XBUTTON1;
        if (event.type == InputType::MouseX2) input.mi.mouseData = XBUTTON2;
        if (release) pressed_buttons.erase(event.type);
        else pressed_buttons.insert(event.type);
    } else if (event.type == InputType::MouseWheel ||
               event.type == InputType::MouseHorizontalWheel) {
        if (event.flags != 0 || event.value1 < -32768 || event.value1 > 32767) return false;
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = event.type == InputType::MouseWheel ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(event.value1);
    } else if (event.type == InputType::Keyboard) {
        if ((event.flags & ~(remoe::protocol::kInputRelease |
                             remoe::protocol::kInputExtendedKey)) != 0 ||
            event.value1 <= 0 || event.value1 > 0x1FF) return false;
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(event.value1);
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (release) input.ki.dwFlags |= KEYEVENTF_KEYUP;
        if (event.flags & remoe::protocol::kInputExtendedKey) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        const std::uint32_t key = static_cast<std::uint32_t>(event.value1) |
            ((event.flags & remoe::protocol::kInputExtendedKey) ? 0x10000u : 0u);
        if (release) pressed_keys.erase(key);
        else pressed_keys.insert(key);
    } else {
        return false;
    }
    if (SendInput(1, &input, sizeof(input)) != 1 && !injection_warning_shown) {
        std::cerr << "Warning: Windows rejected remote input injection; run the host with "
                     "--admin when controlling elevated applications\n";
        injection_warning_shown = true;
    }
    return true;
}

void release_remote_inputs(std::unordered_set<std::uint32_t>& pressed_keys,
                           std::unordered_set<remoe::protocol::InputType>& pressed_buttons) {
    std::vector<INPUT> inputs;
    inputs.reserve(pressed_keys.size() + pressed_buttons.size());
    for (std::uint32_t key : pressed_keys) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = static_cast<WORD>(key & 0xFFFFu);
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (key & 0x10000u) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs.push_back(input);
    }
    for (auto button : pressed_buttons) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = mouse_button_flag(button, true);
        if (button == remoe::protocol::InputType::MouseX1) input.mi.mouseData = XBUTTON1;
        if (button == remoe::protocol::InputType::MouseX2) input.mi.mouseData = XBUTTON2;
        inputs.push_back(input);
    }
    if (!inputs.empty()) SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    pressed_keys.clear();
    pressed_buttons.clear();
}

int run(const Options& options) {
    SetConsoleCtrlHandler(console_handler, TRUE);
#if defined(REMOE_X264_HOST)
    std::cout << "remoe_host_x264 " << REMOE_VERSION << " (protocol v"
              << remoe::protocol::kVersion << ")\n";
    remoe::GdiCapture capture(options.output);
#else
    std::cout << "remoe_host " << REMOE_VERSION << " (protocol v"
              << remoe::protocol::kVersion << ")\n";
    remoe::DesktopCapture capture(options.output);
#endif
    if (options.check_encoder) {
        const std::uint32_t width = scaled_dimension(capture.width(), 100);
        const std::uint32_t height = scaled_dimension(capture.height(), 100);
#if defined(REMOE_X264_HOST)
        auto encoder = remoe::create_x264_h264_encoder(width, height, 30, 8'000'000);
        if (!capture.acquire(std::chrono::milliseconds(250))) {
            throw std::runtime_error("GDI desktop capture failed during H.264 check");
        }
        auto frames = encoder->encode(
            std::span<const std::uint8_t>(
                capture.pixels(), static_cast<std::size_t>(capture.stride()) * capture.height()),
            capture.width(), capture.height(), capture.stride(), true);
        if (frames.empty()) frames = encoder->drain();
        if (frames.empty() || frames.front().data.empty()) {
            throw std::runtime_error("x264 produced no H.264 test frame");
        }
        std::cout << "H.264 encoder check passed: " << frames.front().data.size()
                  << " bytes, " << (frames.front().key_frame ? "key frame" : "frame")
                  << '\n';
        return 0;
#else
        auto encoder = remoe::create_preferred_av1_encoder(
            capture.device(), width, height, 60, 20'000'000);
        capture.use_device(encoder->device());
        for (int attempt = 0; attempt < 10; ++attempt) {
            ID3D11Texture2D* texture = encoder->input_texture();
            if (!capture.acquire(texture, width, height, std::chrono::milliseconds(250))) {
                encoder->discard_input();
                continue;
            }
            auto frames = encoder->encode(true);
            if (frames.empty()) frames = encoder->drain();
            if (frames.empty() || frames.front().data.empty()) {
                throw std::runtime_error("AV1 encoder produced no test frame");
            }
            std::cout << "AV1 encoder check passed: " << frames.front().data.size()
                      << " bytes, " << (frames.front().key_frame ? "key frame" : "frame")
                      << '\n';
            return 0;
        }
        throw std::runtime_error("desktop capture timed out during AV1 encoder check");
#endif
    }
    ensure_firewall_rule();
    std::optional<remoe::ManagedHostIdentity> host_identity;
    std::string signaling_invite;
    if (options.legacy_invite) {
        signaling_invite = remoe::create_webrtc_signaling_invite(options.signaling_url);
    } else {
        std::optional<remoe::ManagedHostIdentity> saved_identity;
        try {
            saved_identity = remoe::load_host_identity();
        } catch (const std::exception& error) {
            if (!options.repair) throw;
            std::cerr << "Ignoring unusable saved Host identity: " << error.what() << '\n';
        }
        if (options.repair || !saved_identity) {
            std::cout << "\nWaiting for account pairing...\n" << std::flush;
            host_identity = remoe::pair_managed_webrtc_host(
                options.signaling_url, options.repair ? saved_identity : std::nullopt,
                [](std::string code) {
                    std::cout << "Pairing code: " << code
                              << "\nEnter this code on the remoe web page within 10 minutes.\n"
                              << std::flush;
                }, [] { return !g_running.load(); });
            remoe::save_host_identity(*host_identity);
            std::cout << "Host paired and credential protected with Windows DPAPI.\n";
        } else {
            host_identity = *saved_identity;
        }
    }
    std::cout << "Display " << options.output << ": " << capture.width() << 'x'
              << capture.height();
#if defined(REMOE_X264_HOST)
    std::wcout << L" (GDI " << capture.device_name() << L")";
#endif
    std::cout << "\nClient FPS limit: ";
    if (options.max_fps) std::cout << options.max_fps;
    else std::cout << "unlimited";
    std::cout << "\nClient bitrate limit: ";
    if (options.max_bitrate_mbps) std::cout << options.max_bitrate_mbps << " Mbps";
    else std::cout << "unlimited";
    std::cout << '\n';
    std::cout << "\nRegistering with signaling server...\n" << std::flush;

    std::uint64_t frame_number = 0;
    const auto epoch = Clock::now();
    bool first_registration = true;
    bool invite_printed = false;
    while (g_running) {
        std::atomic_bool session_running{true};
        std::atomic_bool key_frame_requested{false};
        std::atomic_bool clipboard_enabled{false};
        std::atomic<DWORD> clipboard_sequence{GetClipboardSequenceNumber()};
        std::atomic_uint32_t outbound_clipboard_sequence{0};
        std::mutex input_mutex;
        std::unordered_set<std::uint32_t> pressed_keys;
        std::unordered_set<remoe::protocol::InputType> pressed_buttons;
        bool injection_warning_shown = false;

        struct SessionHandshake {
            std::mutex mutex;
            std::condition_variable changed;
            std::optional<remoe::protocol::ClientConfig> request;
            bool video_open = false;
            bool client_ready = false;
        } handshake;

        remoe::WebRtcTransport::Callbacks control_callbacks;
        control_callbacks.on_local_candidate = [](auto candidate) {
            if (candidate.candidate.find(" typ srflx ") != std::string::npos) {
                std::cout << "WebRTC STUN reflexive candidate gathered\n";
            }
        };
        control_callbacks.on_open = [] {
            std::cout << "WebRTC control DataChannel connected\n";
        };
        control_callbacks.on_video_open = [&] {
            {
                std::lock_guard lock(handshake.mutex);
                handshake.video_open = true;
            }
            handshake.changed.notify_all();
            std::cout << "WebRTC video DataChannel connected\n";
        };
        control_callbacks.on_binary = [&](std::vector<std::uint8_t> message) {
            std::lock_guard lock(input_mutex);
            if (!session_running) return;
            if (message.size() >= sizeof(remoe::protocol::ClipboardHeader)) {
                remoe::protocol::ClipboardHeader header;
                std::memcpy(&header, message.data(), sizeof(header));
                if (header.magic == remoe::protocol::kClipboardMagic) {
                    const bool valid = header.version == remoe::protocol::kVersion &&
                        header.header_size == sizeof(header) &&
                        header.payload_size <= remoe::protocol::kMaxClipboardTextSize &&
                        message.size() == sizeof(header) + header.payload_size &&
                        clipboard_enabled.load();
                    if (!valid) {
                        std::cerr << "Invalid clipboard message over WebRTC\n";
                        session_running = false;
                        handshake.changed.notify_all();
                        return;
                    }
                    const std::string_view text(
                        reinterpret_cast<const char*>(message.data() + sizeof(header)),
                        header.payload_size);
                    if (remoe::write_clipboard_text(text)) {
                        clipboard_sequence = GetClipboardSequenceNumber();
                    } else {
                        std::cerr << "Could not update the Windows clipboard\n";
                    }
                    return;
                }
            }
            if (message.size() == sizeof(remoe::protocol::ClientConfig)) {
                remoe::protocol::ClientConfig request;
                std::memcpy(&request, message.data(), sizeof(request));
                if (request.magic == remoe::protocol::kClientConfigMagic) {
                    {
                        std::lock_guard handshake_lock(handshake.mutex);
                        if (handshake.request) {
                            session_running = false;
                        } else {
                            handshake.request = request;
                        }
                    }
                    handshake.changed.notify_all();
                    return;
                }
            }
            if (message.size() == sizeof(remoe::protocol::StreamReady)) {
                remoe::protocol::StreamReady ready;
                std::memcpy(&ready, message.data(), sizeof(ready));
                if (ready.magic == remoe::protocol::kStreamReadyMagic &&
                    ready.version == remoe::protocol::kVersion &&
                    ready.header_size == sizeof(ready)) {
                    {
                        std::lock_guard handshake_lock(handshake.mutex);
                        handshake.client_ready = true;
                    }
                    handshake.changed.notify_all();
                    return;
                }
            }
            if (message.size() != sizeof(remoe::protocol::InputEvent)) {
                std::cerr << "Invalid WebRTC control message size\n";
                session_running = false;
                handshake.changed.notify_all();
                return;
            }

            remoe::protocol::InputEvent event;
            std::memcpy(&event, message.data(), sizeof(event));
            const bool valid_header = event.magic == remoe::protocol::kInputMagic &&
                event.version == remoe::protocol::kVersion &&
                event.header_size == sizeof(event);
            if (valid_header && event.type == remoe::protocol::InputType::RequestKeyFrame &&
                event.flags == 0 && event.value1 == 0 && event.value2 == 0) {
                key_frame_requested = true;
                return;
            }
            if (!valid_header ||
                !inject_input_event(event, capture.left(), capture.top(), capture.width(),
                                    capture.height(), pressed_keys, pressed_buttons,
                                    injection_warning_shown)) {
                std::cerr << "Invalid remote input event over WebRTC\n";
                session_running = false;
                handshake.changed.notify_all();
            }
        };
        control_callbacks.on_closed = [&] {
            session_running = false;
            handshake.changed.notify_all();
        };
        control_callbacks.on_error = [&](std::string error) {
            std::cerr << "WebRTC control error: " << error << '\n';
            session_running = false;
            handshake.changed.notify_all();
        };

        std::unique_ptr<remoe::WebRtcTransport> control_channel;
        try {
            auto on_registered = [&] {
                if (options.legacy_invite) {
                    if (!invite_printed) {
                        std::cout << "WebRTC invite URL: " << signaling_invite << '\n';
                        invite_printed = true;
                    } else {
                        std::cout << "Signaling reconnected\n";
                    }
                } else if (first_registration) {
                    std::cout << "Host is online in the account device list\n";
                    first_registration = false;
                } else {
                    std::cout << "Signaling reconnected\n";
                }
                std::cout << "Waiting for client (Ctrl+C to stop)\n" << std::flush;
            };
            if (options.legacy_invite) {
                control_channel = remoe::establish_webrtc_over_websocket(
                    remoe::WebRtcTransport::Role::Answerer, signaling_invite,
                    std::move(control_callbacks), (std::chrono::milliseconds::max)(),
                    [] { return !g_running.load(); }, std::move(on_registered));
            } else {
                control_channel = remoe::establish_managed_host_webrtc(
                    options.signaling_url, *host_identity, std::move(control_callbacks),
                    (std::chrono::milliseconds::max)(),
                    [] { return !g_running.load(); }, std::move(on_registered));
            }
        } catch (const std::exception& error) {
            session_running = false;
            if (!g_running) break;
            std::cerr << "Client WebRTC setup failed: " << error.what()
                      << "\nHost remains available\n";
            continue;
        }

        remoe::protocol::ClientConfig request;
        {
            std::unique_lock lock(handshake.mutex);
            while (g_running && session_running &&
                   !(handshake.request.has_value() && handshake.video_open)) {
                handshake.changed.wait_for(lock, std::chrono::milliseconds(250));
            }
            if (!g_running || !session_running) {
                control_channel->close();
                continue;
            }
            request = *handshake.request;
        }

        StreamSettings settings;
        if (!validate_client_settings(request, options, settings)) {
            std::cerr << "Client rejected: invalid protocol v7 stream request\n";
            control_channel->close();
            continue;
        }
        clipboard_enabled = (request.flags & remoe::protocol::kClientClipboardText) != 0;

        const std::uint32_t encoded_width = scaled_dimension(capture.width(), settings.scale_percent);
        const std::uint32_t encoded_height = scaled_dimension(capture.height(), settings.scale_percent);

#if defined(REMOE_X264_HOST)
        auto encoder = remoe::create_x264_h264_encoder(
            encoded_width, encoded_height, settings.fps, settings.bitrate_bps);
#else
        auto encoder = remoe::create_preferred_av1_encoder(
            capture.device(), encoded_width, encoded_height,
            settings.fps, settings.bitrate_bps);
        capture.use_device(encoder->device());
#endif
        std::cout << "Client requested: " << settings.fps << " fps, "
                  << settings.bitrate_bps / 1'000'000.0 << " Mbps, "
                  << settings.scale_percent << "% resolution (" << encoded_width << 'x'
                  << encoded_height << ")\n";

        remoe::protocol::StreamHeader stream_header;
#if defined(REMOE_X264_HOST)
        stream_header.codec = remoe::protocol::kCodecH264;
        stream_header.codec_profile = encoder->profile_level_id();
#endif
        stream_header.width = encoded_width;
        stream_header.height = encoded_height;
        stream_header.fps_num = settings.fps;
        stream_header.bitrate_bps = settings.bitrate_bps;
        if (!control_channel->send_binary(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(&stream_header), sizeof(stream_header)))) {
            continue;
        }

        {
            std::unique_lock lock(handshake.mutex);
            while (g_running && session_running && !handshake.client_ready) {
                handshake.changed.wait_for(lock, std::chrono::milliseconds(250));
            }
        }
        if (!g_running || !session_running) {
            control_channel->close();
            continue;
        }

        bool first_input = true;
        bool first_clipboard_poll = true;
        auto next_frame = Clock::now();
        while (g_running && session_running && control_channel->is_open()) {
            next_frame += std::chrono::microseconds(1'000'000 / settings.fps);
            const DWORD current_clipboard_sequence = GetClipboardSequenceNumber();
            const DWORD previous_clipboard_sequence =
                clipboard_sequence.exchange(current_clipboard_sequence);
            if (clipboard_enabled && (first_clipboard_poll ||
                current_clipboard_sequence != previous_clipboard_sequence)) {
                first_clipboard_poll = false;
                if (auto text = remoe::read_clipboard_text(); text &&
                    !send_clipboard_text(*control_channel, *text,
                                         outbound_clipboard_sequence++)) {
                    session_running = false;
                    break;
                }
            }
#if defined(REMOE_X264_HOST)
            if (!capture.acquire(std::chrono::milliseconds(100))) continue;
#else
            ID3D11Texture2D* texture = encoder->input_texture();
            if (!capture.acquire(texture, encoded_width, encoded_height,
                                 std::chrono::milliseconds(100))) {
                encoder->discard_input();
                continue;
            }
#endif

            const bool force_key_frame =
                first_input || key_frame_requested.exchange(false);
            first_input = false;
#if defined(REMOE_X264_HOST)
            auto packets = encoder->encode(
                std::span<const std::uint8_t>(
                    capture.pixels(),
                    static_cast<std::size_t>(capture.stride()) * capture.height()),
                capture.width(), capture.height(), capture.stride(), force_key_frame);
#else
            auto packets = encoder->encode(force_key_frame);
#endif
            const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - epoch).count();
            bool failed = false;
            for (const auto& packet : packets) {
                const auto result = send_packet(*control_channel, packet, frame_number++,
                                                static_cast<std::uint64_t>(timestamp));
                if (result == VideoSendResult::Dropped) {
                    key_frame_requested = true;
                } else if (result == VideoSendResult::Failed) {
                    failed = true;
                    break;
                }
            }
            if (failed) {
                session_running = false;
                break;
            }
            std::this_thread::sleep_until(next_frame);
        }
        session_running = false;
        control_channel->close();
        {
            std::lock_guard lock(input_mutex);
            release_remote_inputs(pressed_keys, pressed_buttons);
        }
        encoder->drain();
        std::cout << "Client disconnected\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) == "--install-firewall-rule") {
                return install_firewall_rule();
            }
        }
        if (relaunch_as_admin_if_requested()) return 0;
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    return 1;
}
