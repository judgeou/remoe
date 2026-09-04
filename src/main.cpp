#if defined(REMOE_X264_HOST)
#include "gdi_capture.h"
#include "x264_encoder.h"
#else
#include "desktop_capture.h"
#include "video_encoder.h"
#endif
#include "adaptive_stream_controller.h"
#include "clipboard.h"
#include "encoded_video_frame.h"
#include "host_identity.h"
#include "protocol.h"
#include "webrtc_websocket_signaling.h"
#include "windows_service.h"

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

bool send_stream_status(remoe::WebRtcTransport& transport,
                        std::uint32_t media_bitrate_bps,
                        std::uint64_t pacing_bitrate_bps) {
    remoe::protocol::StreamStatus status;
    status.media_bitrate_bps = media_bitrate_bps;
    status.pacing_bitrate_bps = pacing_bitrate_bps;
    return transport.send_binary(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(&status), sizeof(status)));
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
    remoe::protocol::VideoRateControl rate_control =
        remoe::protocol::VideoRateControl::Cbr;
    std::uint32_t quality = 28;
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

std::vector<std::wstring> wide_command_line_arguments() {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) {
        throw std::runtime_error("CommandLineToArgvW failed, Win32 error " +
                                 std::to_string(GetLastError()));
    }
    std::vector<std::wstring> result;
    result.reserve(static_cast<std::size_t>(argument_count));
    for (int index = 0; index < argument_count; ++index) {
        result.emplace_back(arguments[index]);
    }
    LocalFree(arguments);
    return result;
}

bool has_argument(int argc, char** argv, std::string_view requested) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == requested) return true;
    }
    return false;
}

std::vector<std::wstring> service_worker_arguments() {
    const auto arguments = wide_command_line_arguments();
    std::vector<std::wstring> result;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--admin" || argument == L"--install-service" ||
            argument == L"--uninstall-service" || argument == L"--service" ||
            argument == L"--system-worker") {
            continue;
        }
        if (argument == L"--service-stop-event") {
            if (++index >= arguments.size()) {
                throw std::runtime_error("missing value after --service-stop-event");
            }
            continue;
        }
        result.push_back(arguments[index]);
    }
    return result;
}

std::optional<std::wstring> service_stop_event_argument() {
    const auto arguments = wide_command_line_arguments();
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (arguments[index] != L"--service-stop-event") continue;
        if (++index >= arguments.size()) {
            throw std::runtime_error("missing value after --service-stop-event");
        }
        return arguments[index];
    }
    return std::nullopt;
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
        const std::wstring_view argument(arguments[i]);
        if (argument == L"--admin") {
            requested = true;
            continue;
        }
        if (argument == L"--install-service" || argument == L"--uninstall-service") {
            requested = true;
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
        "  --install-service  Install/start an automatic LocalSystem Host service\n"
        "  --uninstall-service Stop and remove the LocalSystem Host service\n"
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
        if (arg == "--admin" || arg == "--install-service" ||
            arg == "--uninstall-service" || arg == "--service" ||
            arg == "--system-worker") {
            continue; // Consumed by the process/service bootstrap.
        }
        if (arg == "--service-stop-event") {
            if (++i >= argc) throw std::runtime_error("missing value after " + std::string(arg));
            continue;
        }
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
                            std::uint64_t timestamp_us) {
    const std::span<const std::uint8_t> encoded_video(packet.data);
    if (encoded_video.empty()) {
        return VideoSendResult::Failed;
    }
    return transport.send_video_frame(encoded_video, timestamp_us)
        ? VideoSendResult::Sent : VideoSendResult::Failed;
}

VideoSendResult send_webcodecs_packet(remoe::WebRtcTransport& transport,
                                      const remoe::EncodedVideoFrame& packet,
                                      std::uint64_t frame_number,
                                      std::uint64_t timestamp_us,
                                      std::size_t maximum_buffered_bytes) {
    const std::span<const std::uint8_t> encoded_video(packet.data);
    if (encoded_video.empty() ||
        encoded_video.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return VideoSendResult::Failed;
    }
    if (transport.video_buffered_amount() > maximum_buffered_bytes) {
        return VideoSendResult::Dropped;
    }

    remoe::protocol::VideoChunkHeader header;
    header.frame_size = static_cast<std::uint32_t>(encoded_video.size());
    header.frame_number = frame_number;
    header.timestamp_us = timestamp_us;
    if (packet.key_frame) header.flags |= remoe::protocol::kFrameKey;
    std::vector<std::uint8_t> message(
        sizeof(header) + remoe::protocol::kVideoChunkPayloadSize);
    for (std::size_t offset = 0; offset < encoded_video.size();
         offset += remoe::protocol::kVideoChunkPayloadSize) {
        const std::size_t chunk_size = (std::min)(
            remoe::protocol::kVideoChunkPayloadSize, encoded_video.size() - offset);
        header.chunk_offset = static_cast<std::uint32_t>(offset);
        std::memcpy(message.data(), &header, sizeof(header));
        std::memcpy(message.data() + sizeof(header), encoded_video.data() + offset,
                    chunk_size);
        if (!transport.send_video_binary(std::span<const std::uint8_t>(
                message.data(), sizeof(header) + chunk_size))) {
            return VideoSendResult::Failed;
        }
    }
    return VideoSendResult::Sent;
}

bool validate_client_settings(const remoe::protocol::ClientConfig& request,
                              const Options& options, StreamSettings& settings) {
    const bool cbr = request.rate_control == remoe::protocol::VideoRateControl::Cbr;
    const bool fixed_quality =
        request.rate_control == remoe::protocol::VideoRateControl::FixedQuality;
    if (request.magic != remoe::protocol::kClientConfigMagic ||
        request.version != remoe::protocol::kVersion ||
        request.header_size != sizeof(request) || request.fps_den != 1 ||
        (request.flags & ~(remoe::protocol::kClientClipboardText |
                           remoe::protocol::kClientStreamStatus |
                           remoe::protocol::kClientLowLatencyVideo)) != 0 ||
        request.fps_num == 0 || request.fps_num > 240 ||
        request.scale_percent < 10 || request.scale_percent > 100 ||
        (options.max_fps != 0 && request.fps_num > options.max_fps) ||
        (!cbr && !fixed_quality) ||
        (cbr && (request.bitrate_bps < 1'000'000u ||
                 request.bitrate_bps > 1'000'000'000u)) ||
        (fixed_quality && (request.bitrate_bps < 1'000'000u ||
                           request.bitrate_bps > 1'000'000'000u ||
                           request.quality < 1 || request.quality > 51)) ||
        (cbr && request.quality != 0) ||
#if defined(REMOE_X264_HOST)
        !cbr || request.bitrate_bps > 50'000'000u ||
#endif
        (options.max_bitrate_mbps != 0 &&
         request.bitrate_bps > options.max_bitrate_mbps * 1'000'000u)) {
        return false;
    }
    settings.fps = request.fps_num;
    settings.bitrate_bps = request.bitrate_bps;
    settings.scale_percent = request.scale_percent;
    settings.rate_control = request.rate_control;
    settings.quality = request.quality;
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
    } else if (event.type == InputType::MouseMoveRelative) {
        if (event.flags != 0 || event.value1 < -32768 || event.value1 > 32767 ||
            event.value2 < -32768 || event.value2 > 32767) return false;
        input.type = INPUT_MOUSE;
        input.mi.dx = event.value1;
        input.mi.dy = event.value2;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
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
    input.mi.dwExtraInfo = static_cast<ULONG_PTR>(
        remoe::protocol::kInjectedInputMarker);
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
        input.ki.dwExtraInfo = static_cast<ULONG_PTR>(
            remoe::protocol::kInjectedInputMarker);
        if (key & 0x10000u) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inputs.push_back(input);
    }
    for (auto button : pressed_buttons) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = mouse_button_flag(button, true);
        input.mi.dwExtraInfo = static_cast<ULONG_PTR>(
            remoe::protocol::kInjectedInputMarker);
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
        if (!encoder->reconfigure_bitrate(4'000'000)) {
            throw std::runtime_error("x264 rejected runtime bitrate reconfiguration");
        }
        std::cout << "H.264 encoder check passed: " << frames.front().data.size()
                  << " bytes, " << (frames.front().key_frame ? "key frame" : "frame")
                  << "; runtime bitrate update passed\n";
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
            if (!encoder->reconfigure_bitrate(10'000'000)) {
                throw std::runtime_error("AV1 encoder rejected runtime bitrate reconfiguration");
            }
            std::cout << "AV1 encoder check passed: " << frames.front().data.size()
                      << " bytes, " << (frames.front().key_frame ? "key frame" : "frame")
                      << "; runtime bitrate update passed\n";
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
        std::atomic<remoe::WebRtcTransport*> active_transport{nullptr};
        std::mutex adaptive_controller_mutex;
        std::shared_ptr<remoe::AdaptiveStreamController> adaptive_controller;

        const auto current_adaptive_controller = [&] {
            std::lock_guard lock(adaptive_controller_mutex);
            return adaptive_controller;
        };

        struct SessionHandshake {
            std::mutex mutex;
            std::condition_variable changed;
            std::optional<remoe::protocol::ClientConfig> request;
            bool video_open = false;
            bool video_data_open = false;
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
            std::cout << "WebRTC standard video track connected\n";
        };
        control_callbacks.on_video_data_open = [&] {
            {
                std::lock_guard lock(handshake.mutex);
                handshake.video_data_open = true;
            }
            handshake.changed.notify_all();
            std::cout << "WebRTC low-latency video DataChannel connected\n";
        };
        control_callbacks.on_video_keyframe_requested = [&] {
            key_frame_requested = true;
        };
        control_callbacks.on_video_feedback = [&](auto feedback) {
            if (auto controller = current_adaptive_controller()) {
                remoe::AdaptiveStreamController::NetworkFeedback observation;
                observation.receiver_report = feedback.receiver_report;
                observation.loss_fraction = feedback.loss_fraction;
                observation.nack_packets = feedback.nack_packets;
                controller->observe_network(observation);
            }
        };
        control_callbacks.on_video_pacing_overflow = [&] {
            key_frame_requested = true;
        };
        control_callbacks.on_binary = [&](std::vector<std::uint8_t> message) {
            const auto host_receive_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - epoch).count());
            std::lock_guard lock(input_mutex);
            if (!session_running) return;
            if (message.size() == sizeof(remoe::protocol::ClockSyncRequest)) {
                remoe::protocol::ClockSyncRequest request;
                std::memcpy(&request, message.data(), sizeof(request));
                auto* transport = active_transport.load();
                if (request.magic == remoe::protocol::kClockSyncMagic &&
                    request.version == remoe::protocol::kVersion &&
                    request.header_size == sizeof(request) && request.reserved == 0 &&
                    transport) {
                    remoe::protocol::ClockSyncResponse response;
                    response.sequence = request.sequence;
                    response.client_send_us = request.client_send_us;
                    response.host_receive_us = host_receive_us;
                    response.host_send_us = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            Clock::now() - epoch).count());
                    if (!transport->send_binary(std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(&response), sizeof(response)))) {
                        session_running = false;
                        handshake.changed.notify_all();
                    }
                    return;
                }
            }
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
            if (valid_header &&
                event.type == remoe::protocol::InputType::RequestKeyFrame) {
                if (event.flags != 0 || event.value1 != 0 || event.value2 != 0) {
                    std::cerr << "Invalid key-frame request over WebRTC\n";
                    session_running = false;
                    handshake.changed.notify_all();
                } else {
                    key_frame_requested = true;
                }
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
                    [] { return !g_running.load(); }, std::move(on_registered),
#if defined(REMOE_X264_HOST)
                    remoe::WebRtcTransport::VideoCodec::H264);
#else
                    remoe::WebRtcTransport::VideoCodec::AV1);
#endif
            } else {
                control_channel = remoe::establish_managed_host_webrtc(
                    options.signaling_url, *host_identity, std::move(control_callbacks),
                    (std::chrono::milliseconds::max)(),
                    [] { return !g_running.load(); }, std::move(on_registered),
#if defined(REMOE_X264_HOST)
                    remoe::WebRtcTransport::VideoCodec::H264);
#else
                    remoe::WebRtcTransport::VideoCodec::AV1);
#endif
            }
            active_transport = control_channel.get();
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
            std::cerr << "Client rejected: invalid protocol v11 stream request\n";
            control_channel->close();
            continue;
        }
        const bool low_latency_video =
            (request.flags & remoe::protocol::kClientLowLatencyVideo) != 0;
        if (low_latency_video) {
            std::unique_lock lock(handshake.mutex);
            while (g_running && session_running && !handshake.video_data_open) {
                handshake.changed.wait_for(lock, std::chrono::milliseconds(250));
            }
            if (!g_running || !session_running) {
                control_channel->close();
                continue;
            }
        }
        clipboard_enabled = (request.flags & remoe::protocol::kClientClipboardText) != 0;
        const bool stream_status_enabled =
            (request.flags & remoe::protocol::kClientStreamStatus) != 0;
        {
            std::lock_guard lock(adaptive_controller_mutex);
            adaptive_controller.reset();
        }

        std::uint32_t working_bitrate_bps = settings.bitrate_bps;
        std::uint64_t pacing_bitrate_bps = low_latency_video ? 0 :
            static_cast<std::uint64_t>(settings.bitrate_bps) * 2u;
        std::chrono::milliseconds pacing_interval{2};
        if (!low_latency_video &&
            settings.rate_control == remoe::protocol::VideoRateControl::Cbr) {
            auto controller = std::make_shared<remoe::AdaptiveStreamController>(
                settings.bitrate_bps);
            const auto initial = controller->initial_decision();
            working_bitrate_bps = initial.media_bitrate_bps;
            pacing_bitrate_bps = initial.pacing_bitrate_bps;
            pacing_interval = initial.pacing_interval;
            {
                std::lock_guard lock(adaptive_controller_mutex);
                adaptive_controller = std::move(controller);
            }
        }
        if (!low_latency_video &&
            (!control_channel->configure_video_pacing(pacing_bitrate_bps) ||
             !control_channel->update_video_pacing(pacing_bitrate_bps, pacing_interval))) {
            std::cerr << "Could not configure the WebRTC RTP video pacer\n";
            control_channel->close();
            continue;
        }
        if (low_latency_video) {
            std::cout << "WebCodecs low-latency channel: immediate frame delivery; media "
                      << working_bitrate_bps / 1'000'000.0 << " Mbps\n";
        } else {
            std::cout << "Adaptive RTP pacing: " << pacing_bitrate_bps / 1'000'000.0
                      << " Mbps, " << pacing_interval.count() << " ms; media starts at "
                      << working_bitrate_bps / 1'000'000.0 << " Mbps, client ceiling "
                      << settings.bitrate_bps / 1'000'000.0 << " Mbps\n";
        }

        const std::uint32_t encoded_width = scaled_dimension(capture.width(), settings.scale_percent);
        const std::uint32_t encoded_height = scaled_dimension(capture.height(), settings.scale_percent);

#if defined(REMOE_X264_HOST)
        auto encoder = remoe::create_x264_h264_encoder(
            encoded_width, encoded_height, settings.fps, working_bitrate_bps);
#else
        auto encoder = remoe::create_preferred_av1_encoder(
            capture.device(), encoded_width, encoded_height,
            settings.fps, working_bitrate_bps,
            settings.rate_control, settings.quality);
        capture.use_device(encoder->device());
#endif
        std::cout << "Client requested: " << settings.fps << " fps, ";
        if (settings.rate_control == remoe::protocol::VideoRateControl::FixedQuality) {
            std::cout << "fixed quality " << settings.quality;
        } else {
            std::cout << settings.bitrate_bps / 1'000'000.0 << " Mbps CBR";
        }
        std::cout << ", " << settings.scale_percent << "% resolution ("
                  << encoded_width << 'x' << encoded_height << ")\n";

        remoe::protocol::StreamHeader stream_header;
#if defined(REMOE_X264_HOST)
        stream_header.codec = remoe::protocol::kCodecH264;
        stream_header.codec_profile = encoder->profile_level_id();
#endif
        stream_header.width = encoded_width;
        stream_header.height = encoded_height;
        stream_header.fps_num = settings.fps;
        stream_header.bitrate_bps = settings.bitrate_bps;
        stream_header.rate_control = settings.rate_control;
        stream_header.quality = settings.quality;
        if (!control_channel->send_binary(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(&stream_header), sizeof(stream_header)))) {
            continue;
        }
        if (stream_status_enabled &&
            !send_stream_status(*control_channel,
                settings.rate_control == remoe::protocol::VideoRateControl::Cbr
                    ? working_bitrate_bps : 0,
                pacing_bitrate_bps)) {
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
        std::uint64_t frame_number = 0;
        const std::size_t low_latency_buffer_limit = (std::clamp)(
            static_cast<std::size_t>(settings.bitrate_bps / 8u / 40u),
            std::size_t{64 * 1024}, std::size_t{512 * 1024});
        const auto frame_interval =
            std::chrono::microseconds(1'000'000 / settings.fps);
        auto next_frame = Clock::now();
        auto next_adaptive_update = Clock::now() + std::chrono::milliseconds(250);
        while (g_running && session_running && control_channel->is_open()) {
            // Desktop Duplication may block until the screen changes.  Do not
            // retain a deadline that became stale while acquire() was waiting:
            // otherwise the next burst of desktop updates is sent without any
            // pacing while the loop tries to catch up with the old schedule.
            const auto now = Clock::now();
            if (now >= next_adaptive_update) {
                next_adaptive_update = now + std::chrono::milliseconds(250);
                if (auto controller = current_adaptive_controller()) {
                    const auto pacing = control_channel->video_pacing_statistics();
                    const auto transport_stats = control_channel->statistics();
                    remoe::AdaptiveStreamController::LocalFeedback local;
                    local.queue_delay_ms = pacing.queue_delay_ms;
                    local.scheduler_lateness_ms = pacing.scheduler_lateness_ms;
                    local.dropped_batches = pacing.dropped_batches;
                    controller->observe_local(local);
                    if (transport_stats.round_trip_time) {
                        remoe::AdaptiveStreamController::NetworkFeedback rtt;
                        rtt.round_trip_time = transport_stats.round_trip_time;
                        controller->observe_network(rtt);
                    }
                    if (auto decision = controller->take_decision()) {
                        if (!encoder->reconfigure_bitrate(decision->media_bitrate_bps)) {
                            std::cerr << "Adaptive bitrate disabled: encoder rejected runtime "
                                         "reconfiguration\n";
                            std::lock_guard lock(adaptive_controller_mutex);
                            adaptive_controller.reset();
                        } else if (!control_channel->update_video_pacing(
                                       decision->pacing_bitrate_bps,
                                       decision->pacing_interval)) {
                            std::cerr << "Adaptive bitrate failed to update RTP pacing\n";
                            session_running = false;
                            break;
                        } else {
                            working_bitrate_bps = decision->media_bitrate_bps;
                            pacing_bitrate_bps = decision->pacing_bitrate_bps;
                            if (decision->force_key_frame) key_frame_requested = true;
                            std::cout << "Adaptive media rate: "
                                      << working_bitrate_bps / 1'000'000.0 << " Mbps; pacer "
                                      << decision->pacing_bitrate_bps / 1'000'000.0 << " Mbps / "
                                      << decision->pacing_interval.count() << " ms ("
                                      << decision->reason << ")\n";
                            if (stream_status_enabled &&
                                !send_stream_status(*control_channel, working_bitrate_bps,
                                                    pacing_bitrate_bps)) {
                                session_running = false;
                                break;
                            }
                        }
                    }
                }
            }
            if (now > next_frame) next_frame = now;
            std::this_thread::sleep_until(next_frame);
            next_frame += frame_interval;

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

            const auto pacing_state = control_channel->video_pacing_statistics();
            // Fixed-quality frames have no bitrate bound and may individually
            // exceed the nominal pacer queue. Finish the current complete frame
            // before capturing the newest desktop state. No key frame is needed:
            // frames skipped before encoding create no RTP or decoder gap.
            if ((!low_latency_video &&
                 settings.rate_control == remoe::protocol::VideoRateControl::FixedQuality &&
                 pacing_state.queued_packets != 0) ||
                (!low_latency_video && pacing_state.queue_delay_ms >= 100.0) ||
                (low_latency_video &&
                 control_channel->video_buffered_amount() > low_latency_buffer_limit)) {
                continue;
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
            // Timestamp the completed capture, before encoding and RTP pacing,
            // so WebRTC's captureTime-based telemetry covers the entire media
            // pipeline instead of starting after the encoder has finished.
            const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - epoch).count();

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
            bool failed = false;
            for (const auto& packet : packets) {
                const auto result = low_latency_video
                    ? send_webcodecs_packet(*control_channel, packet, frame_number++,
                          static_cast<std::uint64_t>(timestamp), low_latency_buffer_limit)
                    : send_packet(*control_channel, packet,
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
        }
        session_running = false;
        control_channel->close();
        {
            std::lock_guard lock(adaptive_controller_mutex);
            adaptive_controller.reset();
        }
        {
            std::lock_guard lock(input_mutex);
            release_remote_inputs(pressed_keys, pressed_buttons);
        }
        encoder->drain();
        std::cout << "Client disconnected\n";
    }
    return 0;
}

int run_service_worker(const Options& options, const std::wstring& stop_event_name) {
    HANDLE stop_event = OpenEventW(SYNCHRONIZE, FALSE, stop_event_name.c_str());
    if (!stop_event) {
        throw std::runtime_error("OpenEventW(service stop) failed, Win32 error " +
                                 std::to_string(GetLastError()));
    }
    HANDLE finished_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished_event) {
        const DWORD error = GetLastError();
        CloseHandle(stop_event);
        throw std::runtime_error("CreateEventW(worker finished) failed, Win32 error " +
                                 std::to_string(error));
    }

    DWORD session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
    std::cout << "Service worker running as LocalSystem in interactive session "
              << session_id << '\n';

    std::thread stop_watcher([stop_event, finished_event] {
        HANDLE events[] = {stop_event, finished_event};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
            g_running = false;
        }
    });

    try {
        const int result = run(options);
        SetEvent(finished_event);
        stop_watcher.join();
        CloseHandle(finished_event);
        CloseHandle(stop_event);
        return result;
    } catch (...) {
        SetEvent(finished_event);
        stop_watcher.join();
        CloseHandle(finished_event);
        CloseHandle(stop_event);
        throw;
    }
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

        const bool install_service = has_argument(argc, argv, "--install-service");
        const bool uninstall_service = has_argument(argc, argv, "--uninstall-service");
        const bool service_process = has_argument(argc, argv, "--service");
        const bool system_worker = has_argument(argc, argv, "--system-worker");
        const int internal_modes = static_cast<int>(install_service) +
            static_cast<int>(uninstall_service) + static_cast<int>(service_process) +
            static_cast<int>(system_worker);
        if (internal_modes > 1) {
            throw std::runtime_error("service management modes cannot be combined");
        }

        if (uninstall_service) return remoe::uninstall_host_service();
        if (install_service) {
            const Options options = parse_options(argc, argv);
            if (options.check_encoder || options.repair || options.legacy_invite) {
                throw std::runtime_error(
                    "--install-service cannot be combined with --check-encoder, --repair, "
                    "or --legacy-invite");
            }
            return remoe::install_host_service(service_worker_arguments());
        }
        if (service_process) {
            if (!remoe::is_running_as_local_system()) {
                throw std::runtime_error("the internal --service mode must be started by Windows SCM");
            }
            return remoe::run_host_service(service_worker_arguments());
        }

        const Options options = parse_options(argc, argv);
        if (system_worker) {
            if (!remoe::is_running_as_local_system()) {
                throw std::runtime_error("the service worker is not running as LocalSystem");
            }
            const auto stop_event = service_stop_event_argument();
            if (!stop_event) {
                throw std::runtime_error("the service worker stop event is missing");
            }
            return run_service_worker(options, *stop_event);
        }
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    return 1;
}
