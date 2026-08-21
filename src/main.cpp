#include "desktop_capture.h"
#include "protocol.h"
#include "tcp_server.h"

#include "NvEncoder/NvEncoderD3D11.h"

#include <Windows.h>

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
    std::string bind = "127.0.0.1";
    std::uint16_t port = 47990;
    std::uint32_t output = 0;
    std::uint32_t fps = 60;
    std::uint32_t bitrate_mbps = 20;
};

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
        "  --bind <address>   Listen address (default: 127.0.0.1)\n"
        "  --port <1-65535>   TCP port (default: 47990)\n"
        "  --output <index>   Desktop output index (default: 0)\n"
        "  --fps <1-240>      Target frame rate (default: 60)\n"
        "  --bitrate <Mbps>   Target bitrate (default: 20)\n"
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
        if (i + 1 >= argc) throw std::runtime_error("missing value after " + std::string(arg));
        const std::string_view value(argv[++i]);
        if (arg == "--bind") options.bind = value;
        else if (arg == "--port") options.port = static_cast<std::uint16_t>(parse_u32(value, arg, 1, 65535));
        else if (arg == "--output") options.output = parse_u32(value, arg, 0, 63);
        else if (arg == "--fps") options.fps = parse_u32(value, arg, 1, 240);
        else if (arg == "--bitrate") options.bitrate_mbps = parse_u32(value, arg, 1, 1000);
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

void configure_encoder(NvEncoderD3D11& encoder, const Options& options) {
    NV_ENC_INITIALIZE_PARAMS init{NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG config{NV_ENC_CONFIG_VER};
    init.encodeConfig = &config;
    encoder.CreateDefaultEncoderParams(&init, NV_ENC_CODEC_AV1_GUID, NV_ENC_PRESET_P1_GUID,
                                       NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);

    init.frameRateNum = options.fps;
    init.frameRateDen = 1;
    init.enablePTD = 1;
    init.encodeConfig->gopLength = options.fps * 2;
    init.encodeConfig->frameIntervalP = 1;
    init.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    init.encodeConfig->rcParams.averageBitRate = options.bitrate_mbps * 1'000'000u;
    init.encodeConfig->rcParams.maxBitRate = init.encodeConfig->rcParams.averageBitRate;
    init.encodeConfig->rcParams.vbvBufferSize = init.encodeConfig->rcParams.averageBitRate / options.fps;
    init.encodeConfig->rcParams.vbvInitialDelay = init.encodeConfig->rcParams.vbvBufferSize;
    init.encodeConfig->encodeCodecConfig.av1Config.repeatSeqHdr = 1;
    encoder.CreateEncoder(&init);
}

int run(const Options& options) {
    SetConsoleCtrlHandler(console_handler, TRUE);
    remoe::WinsockRuntime winsock;
    remoe::DesktopCapture capture(options.output);
    NvEncoderD3D11 encoder(capture.device(), capture.width(), capture.height(),
                           NV_ENC_BUFFER_FORMAT_ARGB, 0);
    configure_encoder(encoder, options);

    remoe::TcpServer server(options.bind, options.port);
    std::cout << "Display " << options.output << ": " << capture.width() << 'x' << capture.height() << '\n'
              << "AV1 NVENC: " << options.fps << " fps, " << options.bitrate_mbps << " Mbps\n"
              << "Listening on " << options.bind << ':' << options.port << " (Ctrl+C to stop)\n";

    std::uint64_t frame_number = 0;
    const auto epoch = Clock::now();
    while (g_running) {
        std::string peer;
        SOCKET accepted = server.accept_client(peer, std::chrono::milliseconds(250));
        if (accepted == INVALID_SOCKET) continue;
        remoe::TcpClient client(accepted);
        std::cout << "Client connected: " << peer << '\n';

        remoe::protocol::StreamHeader stream_header;
        stream_header.width = capture.width();
        stream_header.height = capture.height();
        stream_header.fps_num = options.fps;
        stream_header.bitrate_bps = options.bitrate_mbps * 1'000'000u;
        if (!client.send_all(&stream_header, sizeof(stream_header))) continue;

        bool first_input = true;
        auto next_frame = Clock::now();
        while (g_running && client.connected()) {
            next_frame += std::chrono::microseconds(1'000'000 / options.fps);
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
        std::cout << "Client disconnected\n";
    }

    std::vector<NvEncOutputFrame> pending;
    encoder.EndEncode(pending);
    encoder.DestroyEncoder();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const NVENCException& error) {
        std::cerr << "NVENC error: " << error.what() << " (code " << error.getErrorCode() << ")\n";
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
    }
    return 1;
}
