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

set(BUILD_SHARED_LIBS ON CACHE BOOL "Build libdatachannel as a DLL" FORCE)
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
add_subdirectory("${LIBDATACHANNEL_ROOT}" third_party/libdatachannel)

function(remoe_link_libdatachannel target_name)
    target_link_libraries(${target_name} PRIVATE LibDataChannel::LibDataChannel)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:LibDataChannel::LibDataChannel>
            $<TARGET_FILE_DIR:${target_name}>
        COMMENT "Copying libdatachannel runtime for ${target_name}")
endfunction()

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
target_link_libraries(remoe_webrtc_transport PUBLIC LibDataChannel::LibDataChannel bcrypt)

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
    remoe_webrtc_transport
    d3d11
    dxgi
    shell32
    ws2_32
    "${NVENC_SDK_ROOT}/Lib/win/x64/nvencodeapi.lib")
remoe_link_libdatachannel(remoe_host)

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
        remoe_webrtc_transport
        remoe_vpl_dispatcher
        d3d11
        dxgi
        ws2_32)
    remoe_link_libdatachannel(remoe_client)

    install(TARGETS remoe_client RUNTIME DESTINATION bin)
elseif(REMOE_BUILD_CLIENT)
    message(STATUS "Intel oneVPL source not found at ${VPL_ROOT}; building remoe_host only")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(remoe_webrtc_transport_smoke
        tests/webrtc_transport_smoke.cpp)
    target_compile_definitions(remoe_webrtc_transport_smoke PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00)
    target_compile_options(remoe_webrtc_transport_smoke PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
    target_link_libraries(remoe_webrtc_transport_smoke PRIVATE remoe_webrtc_transport)
    remoe_link_libdatachannel(remoe_webrtc_transport_smoke)
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
    remoe_link_libdatachannel(remoe_webrtc_tcp_bootstrap_smoke)
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
    remoe_link_libdatachannel(remoe_webrtc_websocket_signaling_smoke)
endif()
