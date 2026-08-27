#include "client_identity.h"

#include <windows.h>
#include <dpapi.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace remoe {
namespace {

std::filesystem::path identity_path() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        throw std::runtime_error("could not locate LocalAppData");
    }
    const std::filesystem::path result = std::filesystem::path(raw) / L"remoe" /
                                         L"client-identity.bin";
    CoTaskMemFree(raw);
    return result;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("saved client identity is truncated");
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) value |= std::uint32_t(bytes[offset++]) << shift;
    return value;
}

void append_text(std::vector<std::uint8_t>& bytes, const std::string& value) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::string read_text(const std::vector<std::uint8_t>& bytes, std::size_t& offset) {
    const std::uint32_t size = read_u32(bytes, offset);
    if (size > 16 * 1024 || offset + size > bytes.size()) {
        throw std::runtime_error("saved client identity is invalid");
    }
    std::string result(reinterpret_cast<const char*>(bytes.data() + offset), size);
    offset += size;
    return result;
}

}  // namespace

std::optional<ClientIdentity> load_client_identity() {
    const auto path = identity_path();
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    std::vector<std::uint8_t> encrypted((std::istreambuf_iterator<char>(file)), {});
    if (encrypted.empty() || encrypted.size() > 64 * 1024) return std::nullopt;
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) return std::nullopt;
    std::vector<std::uint8_t> plain(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    try {
        std::size_t offset = 0;
        if (read_u32(plain, offset) != 1) return std::nullopt;
        ClientIdentity result{read_text(plain, offset), read_text(plain, offset)};
        if (offset != plain.size() || result.server_origin.empty() || result.refresh_token.empty()) {
            return std::nullopt;
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void save_client_identity(const ClientIdentity& identity) {
    std::vector<std::uint8_t> plain;
    append_u32(plain, 1);
    append_text(plain, identity.server_origin);
    append_text(plain, identity.refresh_token);
    DATA_BLOB input{static_cast<DWORD>(plain.size()), plain.data()};
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"remoe client session", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw std::runtime_error("could not protect the client session with DPAPI");
    }
    const auto path = identity_path();
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            LocalFree(output.pbData);
            throw std::runtime_error("could not create the client identity file");
        }
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
    }
    LocalFree(output.pbData);
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("could not save the client identity file");
    }
}

void delete_client_identity() noexcept {
    try { DeleteFileW(identity_path().c_str()); } catch (...) {}
}

}  // namespace remoe
