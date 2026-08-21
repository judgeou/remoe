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
    REMOE_VERSION="${PROJECT_VERSION}"
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

# The client uses Intel oneVPL for AV1 hardware decode. Keep the original host-only
# build working when the optional vendored dependency is not present.
set(VPL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvpl"
    CACHE PATH "Path to Intel oneVPL source")
option(REMOE_BUILD_CLIENT "Build the Intel oneVPL AV1 client when available" ON)

if(REMOE_BUILD_CLIENT AND EXISTS "${VPL_ROOT}/api/vpl/mfxvideo.h")
    # Build only the dispatcher. This avoids oneVPL's standalone packaging CMake,
    # which does not support every multi-config generator used by this project.
    add_library(remoe_vpl_dispatcher STATIC
        "${VPL_ROOT}/libvpl/src/windows/main.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_critical_section.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_dispatcher.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_dispatcher_log.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_driver_store_loader.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_dxva2_device.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_function_table.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_library_iterator.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_load_dll.cpp"
        "${VPL_ROOT}/libvpl/src/windows/mfx_win_reg_key.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_dispatcher_vpl.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_dispatcher_vpl_loader.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_dispatcher_vpl_config.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_dispatcher_vpl_lowlatency.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_dispatcher_vpl_log.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_dispatcher_vpl_msdk.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_config_interface/mfx_config_interface.cpp"
        "${VPL_ROOT}/libvpl/src/mfx_config_interface/mfx_config_interface_string_api.cpp")
    target_include_directories(remoe_vpl_dispatcher
        PUBLIC "${VPL_ROOT}/api"
        PRIVATE "${VPL_ROOT}/libvpl")
    target_compile_definitions(remoe_vpl_dispatcher PRIVATE MFX_DEPRECATED_OFF)

    add_executable(remoe_client
        src/client_main.cpp
        src/protocol.h
        src/video_window.cpp
        src/video_window.h
        src/vpl_decoder.cpp
        src/vpl_decoder.h)

    target_include_directories(remoe_client PRIVATE src)
    target_compile_definitions(remoe_client PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00)
    target_compile_options(remoe_client PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
    target_link_libraries(remoe_client PRIVATE
        remoe_vpl_dispatcher
        d3d11
        dxgi
        ws2_32)

    install(TARGETS remoe_client RUNTIME DESTINATION bin)
elseif(REMOE_BUILD_CLIENT)
    message(STATUS "Intel oneVPL source not found at ${VPL_ROOT}; building remoe_host only")
endif()
