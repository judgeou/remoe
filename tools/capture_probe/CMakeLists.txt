cmake_minimum_required(VERSION 3.24)

project(remoe_capture_probe LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "remoe_capture_probe supports Windows only")
endif()

add_executable(remoe_capture_probe capture_probe.cpp)
set_property(TARGET remoe_capture_probe PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(remoe_capture_probe PRIVATE cxx_std_20)
target_compile_definitions(remoe_capture_probe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0602)
target_compile_options(remoe_capture_probe PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>)
target_link_libraries(remoe_capture_probe PRIVATE d3d11 dxgi gdi32 user32)
