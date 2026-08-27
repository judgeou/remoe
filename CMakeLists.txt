cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0144 NEW)
endif()

project(remoe_host VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "remoe_host currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBDATACHANNEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel-0.24.5"
    CACHE PATH "Path to libdatachannel source")
set(MBEDTLS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-3.6.7"
    CACHE PATH "Path to Mbed TLS source used by libdatachannel")

foreach(required_file
        "${LIBDATACHANNEL_ROOT}/CMakeLists.txt"
        "${LIBDATACHANNEL_ROOT}/include/rtc/rtc.hpp"
        "${LIBDATACHANNEL_ROOT}/deps/libjuice/CMakeLists.txt"
        "${LIBDATACHANNEL_ROOT}/deps/libsrtp/CMakeLists.txt"
        "${LIBDATACHANNEL_ROOT}/deps/plog/CMakeLists.txt"
        "${LIBDATACHANNEL_ROOT}/deps/usrsctp/CMakeLists.txt"
        "${MBEDTLS_ROOT}/CMakeLists.txt")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing libdatachannel dependency file: ${required_file}")
    endif()
endforeach()

# Build the cryptographic backend in-tree so developers do not need a separate
# OpenSSL SDK. Only the three Mbed TLS libraries required by libdatachannel are
# enabled; its command-line programs and tests remain out of the remoe build.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/remoe_mbedtls_user_config.h"
    CACHE FILEPATH "Mbed TLS features required by libdatachannel" FORCE)
add_subdirectory("${MBEDTLS_ROOT}" third_party/mbedtls EXCLUDE_FROM_ALL)

# libdatachannel and its bundled libSRTP use two generations of Mbed TLS CMake
# target/variable names. Provide both without modifying the vendored projects.
add_library(MbedTLS::MbedTLS ALIAS mbedtls)
add_library(MbedTLS::MbedX509 ALIAS mbedx509)
add_library(MbedTLS::MbedCrypto ALIAS mbedcrypto)
set(MbedTLS_INCLUDE_DIR "${MBEDTLS_ROOT}/include" CACHE PATH "" FORCE)
set(MbedTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MbedX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MbedCrypto_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(MBEDTLS_INCLUDE_DIRS "${MBEDTLS_ROOT}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(MBEDTLS_LIBRARIES "mbedtls;mbedx509;mbedcrypto" CACHE STRING "" FORCE)
# Ensure libSRTP resolves its own compatibility FindMbedTLS module. The newer
# libdatachannel module attempts to mutate imported targets and is intended for
# installed Mbed TLS packages rather than add_subdirectory() targets.
list(PREPEND CMAKE_MODULE_PATH "${LIBDATACHANNEL_ROOT}/deps/libsrtp/cmake")

# Keep libdatachannel's shared-library configuration enabled so its vendored
# dependency install rules stay disabled, but link only its explicit static
# target below. Excluding the shared target from ALL prevents datachannel.dll
# from being built or copied into the application directory.
set(BUILD_SHARED_LIBS ON CACHE BOOL "Configure libdatachannel shared target" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(PREFER_SYSTEM_LIB OFF CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(LIBSRTP_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBDATACHANNEL_ROOT}" third_party/libdatachannel EXCLUDE_FROM_ALL)
set_target_properties(datachannel PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_library(remoe_webrtc_transport STATIC
    src/webrtc_tcp_bootstrap.cpp
    src/webrtc_tcp_bootstrap.h
    src/webrtc_transport.cpp
    src/webrtc_transport.h
    src/webrtc_websocket_signaling.cpp
    src/webrtc_websocket_signaling.h)
target_include_directories(remoe_webrtc_transport PUBLIC src)
target_compile_definitions(remoe_webrtc_transport PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00)
target_compile_options(remoe_webrtc_transport PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
target_link_libraries(remoe_webrtc_transport
    PUBLIC LibDataChannel::LibDataChannelStatic bcrypt crypt32)

# The oneVPL dispatcher is shared by the host encoder and optional native client
# decoder. If the vendored source is absent, the host remains buildable with its
# NVENC fallback.
set(VPL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvpl"
    CACHE PATH "Path to Intel oneVPL source")
option(REMOE_BUILD_CLIENT "Build the Intel oneVPL AV1 client when available" ON)
option(REMOE_BUILD_X264_HOST "Build the GPL GDI/libyuv/x264 host" OFF)

if(EXISTS "${VPL_ROOT}/api/vpl/mfxvideo.h")
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
    target_compile_definitions(remoe_vpl_dispatcher PRIVATE
        MFX_DEPRECATED_OFF
        ONEVPL_EXPERIMENTAL)
endif()

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
    src/clipboard.cpp
    src/clipboard.h
    src/host_identity.cpp
    src/host_identity.h
    src/main.cpp
    src/desktop_capture.cpp
    src/desktop_capture.h
    src/protocol.h
    src/video_encoder.cpp
    src/video_encoder.h
    src/nvenc_api_loader.cpp
    src/nvenc_encoder.cpp
    "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoder.cpp"
    "${NVENC_SAMPLE_DIR}/NvEncoder/NvEncoderD3D11.cpp")

if(TARGET remoe_vpl_dispatcher)
    target_sources(remoe_host PRIVATE src/vpl_encoder.cpp)
    target_compile_definitions(remoe_host PRIVATE
        REMOE_HAS_VPL_ENCODER=1
        ONEVPL_EXPERIMENTAL)
    target_link_libraries(remoe_host PRIVATE remoe_vpl_dispatcher)
else()
    message(WARNING "Intel oneVPL source not found at ${VPL_ROOT}; remoe_host will use NVENC only")
endif()

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
    remoe_webrtc_transport
    d3d11
    dxgi
    ole32
    oleaut32
    shell32
    ws2_32)
install(TARGETS remoe_host RUNTIME DESTINATION bin)

if(REMOE_BUILD_X264_HOST)
    set(LIBYUV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libyuv"
        CACHE PATH "Path to libyuv source")
    set(X264_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/x264"
        CACHE PATH "Path to x264 source")
    set(X264_BUILD_ROOT "${X264_ROOT}/build-msvc"
        CACHE PATH "Path to configured x264 MSVC build")
    set(X264_LIBRARY "${X264_BUILD_ROOT}/libx264.lib"
        CACHE FILEPATH "Path to the static x264 library")

    foreach(required_file
            "${LIBYUV_ROOT}/CMakeLists.txt"
            "${LIBYUV_ROOT}/include/libyuv.h"
            "${X264_ROOT}/x264.h"
            "${X264_BUILD_ROOT}/x264_config.h"
            "${X264_LIBRARY}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR
                "Missing x264 host dependency file: ${required_file}. "
                "See README.md for the separate GPL build instructions.")
        endif()
    endforeach()

    set(UNIT_TEST OFF CACHE BOOL "Build libyuv unit tests" FORCE)
    set(CMAKE_DISABLE_FIND_PACKAGE_JPEG TRUE CACHE BOOL
        "The remoe x264 path does not use libyuv MJPEG conversion" FORCE)
    add_subdirectory("${LIBYUV_ROOT}" third_party/libyuv EXCLUDE_FROM_ALL)

    add_library(remoe_x264 STATIC IMPORTED GLOBAL)
    set_target_properties(remoe_x264 PROPERTIES
        IMPORTED_LOCATION "${X264_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${X264_ROOT};${X264_BUILD_ROOT}")

    add_executable(remoe_host_x264
        src/remoe_host.manifest
        src/clipboard.cpp
        src/clipboard.h
        src/encoded_video_frame.h
        src/gdi_capture.cpp
        src/gdi_capture.h
        src/host_identity.cpp
        src/host_identity.h
        src/main.cpp
        src/protocol.h
        src/x264_encoder.cpp
        src/x264_encoder.h)
    target_include_directories(remoe_host_x264 PRIVATE
        src
        "${LIBYUV_ROOT}/include")
    target_compile_definitions(remoe_host_x264 PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        REMOE_VERSION="${PROJECT_VERSION}"
        REMOE_X264_HOST=1
        _WIN32_WINNT=0x0A00)
    target_compile_options(remoe_host_x264 PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
    target_link_options(remoe_host_x264 PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/MANIFESTUAC:NO>)
    target_link_libraries(remoe_host_x264 PRIVATE
        remoe_webrtc_transport
        yuv
        remoe_x264
        gdi32
        ole32
        oleaut32
        shell32
        user32
        ws2_32)
    install(TARGETS remoe_host_x264 RUNTIME DESTINATION bin)
    install(FILES "${X264_ROOT}/COPYING" DESTINATION licenses/x264)
    install(FILES "${LIBYUV_ROOT}/LICENSE" DESTINATION licenses/libyuv)
endif()

if(REMOE_BUILD_CLIENT AND TARGET remoe_vpl_dispatcher)
    add_executable(remoe_client
        src/clipboard.cpp
        src/clipboard.h
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
        remoe_webrtc_transport
        remoe_vpl_dispatcher
        d3d11
        dxgi)
    install(TARGETS remoe_client RUNTIME DESTINATION bin)
elseif(REMOE_BUILD_CLIENT)
    message(STATUS "Intel oneVPL source not found at ${VPL_ROOT}; skipping remoe_client")
endif()

include(CTest)
if(BUILD_TESTING)
    if(REMOE_BUILD_X264_HOST)
        add_executable(remoe_x264_encoder_smoke
            tests/x264_encoder_smoke.cpp
            src/x264_encoder.cpp
            src/x264_encoder.h
            src/encoded_video_frame.h)
        target_include_directories(remoe_x264_encoder_smoke PRIVATE
            src
            "${LIBYUV_ROOT}/include")
        target_compile_options(remoe_x264_encoder_smoke PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
        target_link_libraries(remoe_x264_encoder_smoke PRIVATE yuv remoe_x264)
        add_test(NAME x264_encoder_smoke COMMAND remoe_x264_encoder_smoke)
        set_tests_properties(x264_encoder_smoke PROPERTIES TIMEOUT 30)
    endif()

    add_executable(remoe_webrtc_transport_smoke
        tests/webrtc_transport_smoke.cpp)
    target_compile_definitions(remoe_webrtc_transport_smoke PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00)
    target_compile_options(remoe_webrtc_transport_smoke PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
    target_link_libraries(remoe_webrtc_transport_smoke PRIVATE remoe_webrtc_transport)
    add_test(NAME webrtc_transport_smoke COMMAND remoe_webrtc_transport_smoke)
    set_tests_properties(webrtc_transport_smoke PROPERTIES TIMEOUT 30)

    add_executable(remoe_webrtc_tcp_bootstrap_smoke
        tests/webrtc_tcp_bootstrap_smoke.cpp)
    target_compile_definitions(remoe_webrtc_tcp_bootstrap_smoke PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00)
    target_compile_options(remoe_webrtc_tcp_bootstrap_smoke PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
    target_link_libraries(remoe_webrtc_tcp_bootstrap_smoke PRIVATE remoe_webrtc_transport)
    add_test(NAME webrtc_tcp_bootstrap_smoke COMMAND remoe_webrtc_tcp_bootstrap_smoke)
    set_tests_properties(webrtc_tcp_bootstrap_smoke PROPERTIES TIMEOUT 30)

    add_executable(remoe_webrtc_websocket_signaling_smoke
        tests/webrtc_websocket_signaling_smoke.cpp)
    target_compile_definitions(remoe_webrtc_websocket_signaling_smoke PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00)
    target_compile_options(remoe_webrtc_websocket_signaling_smoke PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
    target_link_libraries(remoe_webrtc_websocket_signaling_smoke PRIVATE remoe_webrtc_transport)
endif()
