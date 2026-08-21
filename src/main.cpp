#include "desktop_capture.h"
#include "protocol.h"
#include "tcp_server.h"

#include "NvEncoder/NvEncoderD3D11.h"

#include <Windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
std::atomic_bool g_running{true};

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

struct Options {
    std::string bind = "localhost";
    std::uint16_t port = 47990;
    std::uint32_t output = 0;
    // Zero means the operator did not configure a server-side limit.
    std::uint32_t max_fps = 0;
    std::uint32_t max_bitrate_mbps = 0;
};

struct StreamSettings {
    std::uint32_t fps = 60;
    std::uint32_t bitrate_bps = 20'000'000;
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

    std::vector<wchar_t> executable(32768);
    const DWORD executable_size = GetModuleFileNameW(nullptr, executable.data(),
                                                     static_cast<DWORD>(executable.size()));
    if (executable_size == 0 || executable_size == executable.size()) {
        throw std::runtime_error("GetModuleFileNameW failed, Win32 error " +
                                 std::to_string(GetLastError()));
    }

    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.data();
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
    std::cout <<
        "remoe_host - Windows desktop AV1/NVENC streaming host\n\n"
        "Usage: remoe_host [options]\n"
        "  --bind <address>   Listen address (default: localhost, IPv4 + IPv6)\n"
        "  --port <1-65535>   TCP port (default: 47990)\n"
        "  --output <index>   Desktop output index (default: 0)\n"
        "  --max-fps <1-240>  Optional maximum client frame rate (default: unlimited)\n"
        "  --max-bitrate <Mbps> Optional maximum client bitrate (default: unlimited)\n"
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
        if (i + 1 >= argc) throw std::runtime_error("missing value after " + std::string(arg));
        const std::string_view value(argv[++i]);
        if (arg == "--bind") options.bind = value;
        else if (arg == "--port") options.port = static_cast<std::uint16_t>(parse_u32(value, arg, 1, 65535));
        else if (arg == "--output") options.output = parse_u32(value, arg, 0, 63);
        else if (arg == "--max-fps" || arg == "--fps") {
            options.max_fps = parse_u32(value, arg, 1, 240);
        } else if (arg == "--max-bitrate" || arg == "--bitrate") {
            options.max_bitrate_mbps = parse_u32(value, arg, 1, 1000);
        }
        else throw std::runtime_error("unknown option: " + std::string(arg));
    }
    return options;
}

bool is_key_picture(NV_ENC_PIC_TYPE type) {
    return type == NV_ENC_PIC_TYPE_IDR || type == NV_ENC_PIC_TYPE_I ||
           type == NV_ENC_PIC_TYPE_SWITCH;
}

std::uint32_t read_le32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::span<const std::uint8_t> unwrap_av1_ivf(const std::vector<std::uint8_t>& packet) {
    std::size_t offset = 0;
    if (packet.size() >= 4 && packet[0] == 'D' && packet[1] == 'K' &&
        packet[2] == 'I' && packet[3] == 'F') {
        if (packet.size() < 32) throw std::runtime_error("truncated IVF file header from NVENC wrapper");
        offset = 32;
    }
    if (packet.size() < offset + 12) {
        throw std::runtime_error("truncated IVF frame header from NVENC wrapper");
    }
    const std::uint32_t frame_size = read_le32(packet.data() + offset);
    offset += 12;
    if (frame_size != packet.size() - offset) {
        throw std::runtime_error("invalid IVF frame size from NVENC wrapper");
    }
    return {packet.data() + offset, frame_size};
}

bool send_packet(remoe::TcpClient& client, const NvEncOutputFrame& packet,
                 std::uint64_t frame_number, std::uint64_t timestamp_us) {
    const auto av1 = unwrap_av1_ivf(packet.frame);
    if (av1.size() > (std::numeric_limits<std::uint32_t>::max)()) return false;
    remoe::protocol::FrameHeader header;
    header.payload_size = static_cast<std::uint32_t>(av1.size());
    header.frame_number = frame_number;
    header.timestamp_us = timestamp_us;
    if (is_key_picture(packet.pictureType)) header.flags |= remoe::protocol::kFrameKey;
    return client.send_all(&header, sizeof(header)) &&
           client.send_all(av1.data(), av1.size());
}

void configure_encoder(NvEncoderD3D11& encoder, const StreamSettings& settings) {
    NV_ENC_INITIALIZE_PARAMS init{NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG config{NV_ENC_CONFIG_VER};
    init.encodeConfig = &config;
    encoder.CreateDefaultEncoderParams(&init, NV_ENC_CODEC_AV1_GUID, NV_ENC_PRESET_P1_GUID,
                                       NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

    init.frameRateNum = settings.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig->gopLength = settings.fps * 2;
    init.encodeConfig->frameIntervalP = 1;
    init.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    init.encodeConfig->rcParams.averageBitRate = settings.bitrate_bps;
    init.encodeConfig->rcParams.maxBitRate = init.encodeConfig->rcParams.averageBitRate;
    init.encodeConfig->rcParams.vbvBufferSize = init.encodeConfig->rcParams.averageBitRate / settings.fps;
    init.encodeConfig->rcParams.vbvInitialDelay = init.encodeConfig->rcParams.vbvBufferSize;
    init.encodeConfig->encodeCodecConfig.av1Config.repeatSeqHdr = 1;
    encoder.CreateEncoder(&init);
}

bool receive_client_settings(remoe::TcpClient& client, const Options& options,
                             StreamSettings& settings) {
    remoe::protocol::ClientConfig request;
    if (!client.receive_all(&request, sizeof(request))) return false;
    if (request.magic != remoe::protocol::kClientConfigMagic ||
        request.version != remoe::protocol::kVersion ||
        request.header_size != sizeof(request) || request.fps_den != 1 ||
        request.reserved != 0 ||
        request.fps_num == 0 || request.fps_num > 240 ||
        (options.max_fps != 0 && request.fps_num > options.max_fps) ||
        request.bitrate_bps < 1'000'000u ||
        request.bitrate_bps > 1'000'000'000u ||
        (options.max_bitrate_mbps != 0 &&
         request.bitrate_bps > options.max_bitrate_mbps * 1'000'000u)) {
        return false;
    }
    settings.fps = request.fps_num;
    settings.bitrate_bps = request.bitrate_bps;
    return true;
}

int run(const Options& options) {
    SetConsoleCtrlHandler(console_handler, TRUE);
    remoe::WinsockRuntime winsock;
    remoe::DesktopCapture capture(options.output);

    remoe::TcpServer server(options.bind, options.port);
    std::cout << "Display " << options.output << ": " << capture.width() << 'x' << capture.height() << '\n'
              << "Client FPS limit: ";
    if (options.max_fps) std::cout << options.max_fps;
    else std::cout << "unlimited";
    std::cout << "\nClient bitrate limit: ";
    if (options.max_bitrate_mbps) std::cout << options.max_bitrate_mbps << " Mbps";
    else std::cout << "unlimited";
    std::cout << '\n'
              << "Listening on " << options.bind << ':' << options.port << " (Ctrl+C to stop)\n";

    std::uint64_t frame_number = 0;
    const auto epoch = Clock::now();
    while (g_running) {
        std::string peer;
        SOCKET accepted = server.accept_client(peer, std::chrono::milliseconds(250));
        if (accepted == INVALID_SOCKET) continue;
        remoe::TcpClient client(accepted);
        std::cout << "Client connected: " << peer << '\n';

        StreamSettings settings;
        if (!receive_client_settings(client, options, settings)) {
            std::cout << "Client rejected: invalid or missing protocol v2 stream request\n";
            continue;
        }

        NvEncoderD3D11 encoder(capture.device(), capture.width(), capture.height(),
                               NV_ENC_BUFFER_FORMAT_ARGB, 0);
        configure_encoder(encoder, settings);
        std::cout << "Client requested: " << settings.fps << " fps, "
                  << settings.bitrate_bps / 1'000'000.0 << " Mbps\n";

        remoe::protocol::StreamHeader stream_header;
        stream_header.width = capture.width();
        stream_header.height = capture.height();
        stream_header.fps_num = settings.fps;
        stream_header.bitrate_bps = settings.bitrate_bps;
        if (!client.send_all(&stream_header, sizeof(stream_header))) {
            encoder.DestroyEncoder();
            continue;
        }

        bool first_input = true;
        auto next_frame = Clock::now();
        while (g_running && client.connected()) {
            next_frame += std::chrono::microseconds(1'000'000 / settings.fps);
            const NvEncInputFrame* input = encoder.GetNextInputFrame();
            auto* texture = static_cast<ID3D11Texture2D*>(input->inputPtr);
            if (!capture.acquire(texture, std::chrono::milliseconds(100))) continue;

            NV_ENC_PIC_PARAMS picture{NV_ENC_PIC_PARAMS_VER};
            if (first_input) {
                picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
                first_input = false;
            }

            std::vector<NvEncOutputFrame> packets;
            encoder.EncodeFrame(packets, &picture);
            const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - epoch).count();
            bool sent = true;
            for (const auto& packet : packets) {
                if (!send_packet(client, packet, frame_number++, static_cast<std::uint64_t>(timestamp))) {
                    sent = false;
                    break;
                }
            }
            if (!sent) break;
            std::this_thread::sleep_until(next_frame);
        }
        client.close();
        std::vector<NvEncOutputFrame> pending;
        encoder.EndEncode(pending);
        encoder.DestroyEncoder();
        std::cout << "Client disconnected\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (relaunch_as_admin_if_requested()) return 0;
        return run(parse_options(argc, argv));
    } catch (const NVENCException& error) {
        std::cerr << "NVENC error: " << error.what() << " (code " << error.getErrorCode() << ")\n";
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    return 1;
}
