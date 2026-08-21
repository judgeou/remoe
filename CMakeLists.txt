cmake_minimum_required(VERSION 3.24)

project(remoe_host VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "remoe_host currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NVENC_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/NVENC_Video_Codec_SDK_13.0.37"
    CACHE PATH "Path to NVIDIA Video Codec SDK")
set(NVENC_INTERFACE_DIR "${NVENC_SDK_ROOT}/Interface")
set(NVENC_SAMPLE_DIR "${NVENC_SDK_ROOT}/Samples/NvCodec")

foreach(required_file
        "${NVENC_INTERFACE_DIR}/nvEncodeAPI.h"
        "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoder.h"
        "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoder.cpp"
        "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoderD3D11.cpp")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing NVENC SDK file: ${required_file}")
    endif()
endforeach()

add_executable(remoe_host
    src/remoe_host.manifest
    src/main.cpp
    src/desktop_capture.cpp
    src/desktop_capture.h
    src/protocol.h
    src/tcp_server.cpp
    src/tcp_server.h
    "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoder.cpp"
    "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoderD3D11.cpp")

target_include_directories(remoe_host PRIVATE
    src
    "${NVENC_INTERFACE_DIR}"
    "${NVENC_SAMPLE_DIR}"
    "${NVENC_SDK_ROOT}/Samples/Utils")

target_compile_definitions(remoe_host PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00)

target_compile_options(remoe_host PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)

target_link_options(remoe_host PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/MANIFESTUAC:NO>)

target_link_libraries(remoe_host PRIVATE
    d3d11
    dxgi
    shell32
    ws2_32
    "${NVENC_SDK_ROOT}/Lib/win/x64/nvencodeapi.lib")

install(TARGETS remoe_host RUNTIME DESTINATION bin)
