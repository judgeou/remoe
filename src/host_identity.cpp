#include "host_identity.h"

#include <Windows.h>
#include <dpapi.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace remoe {
namespace {

std::filesystem::path identity_path() {
    std::wstring local_app_data(32768, L'\0');
    const DWORD size = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
    if (size == 0 || size >= local_app_data.size()) {
        throw std::runtime_error("LOCALAPPDATA is unavailable");
    }
    local_app_data.resize(size);
    return std::filesystem::path(local_app_data) / L"remoe" / L"host-identity.bin";
}

DATA_BLOB blob_for(std::vector<std::uint8_t>& bytes) {
    return {
        static_cast<DWORD>(bytes.size()),
        reinterpret_cast<BYTE*>(bytes.data()),
    };
}

} // namespace

std::optional<ManagedHostIdentity> load_host_identity() {
    const auto path = identity_path();
    if (!std::filesystem::exists(path)) return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Failed to open the saved Host identity");
    std::vector<std::uint8_t> encrypted(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (encrypted.empty()) throw std::runtime_error("The saved Host identity is empty");

    DATA_BLOB encrypted_blob = blob_for(encrypted);
    DATA_BLOB clear_blob{};
    if (!CryptUnprotectData(&encrypted_blob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &clear_blob)) {
        throw std::runtime_error(
            "The saved Host identity cannot be decrypted; run with --repair");
    }
    std::string clear(reinterpret_cast<const char*>(clear_blob.pbData), clear_blob.cbData);
    LocalFree(clear_blob.pbData);
    const std::size_t separator = clear.find('\n');
    if (separator == std::string::npos) {
        throw std::runtime_error("The saved Host identity is invalid; run with --repair");
    }
    ManagedHostIdentity identity{clear.substr(0, separator), clear.substr(separator + 1)};
    if (!valid_managed_host_identity(identity)) {
        throw std::runtime_error("The saved Host identity is invalid; run with --repair");
    }
    return identity;
}

void save_host_identity(const ManagedHostIdentity& identity) {
    if (!valid_managed_host_identity(identity)) {
        throw std::invalid_argument("Cannot save an invalid Host identity");
    }
    std::string clear = identity.device_id + '\n' + identity.token;
    std::vector<std::uint8_t> clear_bytes(clear.begin(), clear.end());
    DATA_BLOB clear_blob = blob_for(clear_bytes);
    DATA_BLOB encrypted_blob{};
    if (!CryptProtectData(&clear_blob, L"remoe Host identity", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted_blob)) {
        throw std::runtime_error("Failed to protect the Host identity with Windows DPAPI");
    }

    const auto path = identity_path();
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encrypted_blob.pbData), encrypted_blob.cbData);
        if (!output) {
            LocalFree(encrypted_blob.pbData);
            throw std::runtime_error("Failed to write the protected Host identity");
        }
    }
    LocalFree(encrypted_blob.pbData);
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Failed to activate the protected Host identity");
    }
}

} // namespace remoe
