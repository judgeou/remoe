#include "nvEncodeAPI.h"

#include <Windows.h>

#include <cstdint>
#include <mutex>

namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using GetMaxVersionFn = NVENCSTATUS(NVENCAPI*)(std::uint32_t*);

HMODULE g_nvenc_module = nullptr;
CreateInstanceFn g_create_instance = nullptr;
GetMaxVersionFn g_get_max_version = nullptr;
std::once_flag g_load_once;

void load_nvenc() {
#if defined(_WIN64)
    constexpr wchar_t library_name[] = L"nvEncodeAPI64.dll";
#else
    constexpr wchar_t library_name[] = L"nvEncodeAPI.dll";
#endif
    g_nvenc_module = LoadLibraryW(library_name);
    if (!g_nvenc_module) return;
    g_create_instance = reinterpret_cast<CreateInstanceFn>(
        GetProcAddress(g_nvenc_module, "NvEncodeAPICreateInstance"));
    g_get_max_version = reinterpret_cast<GetMaxVersionFn>(
        GetProcAddress(g_nvenc_module, "NvEncodeAPIGetMaxSupportedVersion"));
    if (!g_create_instance || !g_get_max_version) {
        g_create_instance = nullptr;
        g_get_max_version = nullptr;
        FreeLibrary(g_nvenc_module);
        g_nvenc_module = nullptr;
    }
}

bool ensure_nvenc_loaded() {
    std::call_once(g_load_once, load_nvenc);
    return g_nvenc_module && g_create_instance && g_get_max_version;
}

} // namespace

extern "C" NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(std::uint32_t* version) {
    if (!version) return NV_ENC_ERR_INVALID_PTR;
    if (!ensure_nvenc_loaded()) return NV_ENC_ERR_NO_ENCODE_DEVICE;
    return g_get_max_version(version);
}

extern "C" NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(
    NV_ENCODE_API_FUNCTION_LIST* function_list) {
    if (!function_list) return NV_ENC_ERR_INVALID_PTR;
    if (!ensure_nvenc_loaded()) return NV_ENC_ERR_NO_ENCODE_DEVICE;
    return g_create_instance(function_list);
}
