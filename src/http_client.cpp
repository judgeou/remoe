#include "http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>

namespace remoe {
namespace {

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle() { if (handle_) WinHttpCloseHandle(handle_); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const noexcept { return handle_; }
private:
    HINTERNET handle_ = nullptr;
};

[[noreturn]] void throw_winhttp(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed (WinHTTP " +
                             std::to_string(GetLastError()) + ")");
}

}  // namespace

HttpResponse http_request(std::string_view url, std::wstring_view method,
                          std::string_view json_body, std::string_view bearer_token) {
    const std::wstring wide_url = widen(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) throw_winhttp("WinHttpCrackUrl");
    if (components.nScheme != INTERNET_SCHEME_HTTPS &&
        components.nScheme != INTERNET_SCHEME_HTTP) {
        throw std::runtime_error("server URL must use https:// (http:// is allowed for local testing)");
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    if (components.nScheme == INTERNET_SCHEME_HTTP && host != L"localhost" &&
        host != L"127.0.0.1" && host != L"::1") {
        throw std::runtime_error("unencrypted HTTP is only allowed for localhost");
    }
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    InternetHandle session(WinHttpOpen(L"remoe-client/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) throw_winhttp("WinHttpOpen");
    WinHttpSetTimeouts(session.get(), 10'000, 10'000, 15'000, 15'000);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), components.nPort, 0));
    if (!connection.get()) throw_winhttp("WinHttpConnect");
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.get(), std::wstring(method).c_str(),
                                              path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.get()) throw_winhttp("WinHttpOpenRequest");

    std::wstring headers = L"Accept: application/json\r\n";
    if (!json_body.empty() || method == L"POST") headers += L"Content-Type: application/json\r\n";
    if (!bearer_token.empty()) headers += L"Authorization: Bearer " + widen(bearer_token) + L"\r\n";
    void* body = json_body.empty() ? WINHTTP_NO_REQUEST_DATA
                                   : const_cast<char*>(json_body.data());
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1), body,
                            static_cast<DWORD>(json_body.size()),
                            static_cast<DWORD>(json_body.size()), 0)) {
        throw_winhttp("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) throw_winhttp("WinHttpReceiveResponse");

    HttpResponse response;
    DWORD status_size = sizeof(response.status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp("WinHttpQueryHeaders");
    }
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) throw_winhttp("WinHttpQueryDataAvailable");
        if (!available) break;
        const std::size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read)) {
            throw_winhttp("WinHttpReadData");
        }
        response.body.resize(offset + read);
    }
    return response;
}

std::string normalize_server_origin(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (value.starts_with("wss://")) value.replace(0, 3, "https");
    else if (value.starts_with("ws://")) value.replace(0, 2, "http");
    if (!value.starts_with("https://") && !value.starts_with("http://")) {
        throw std::invalid_argument("server address must start with https://");
    }
    const std::size_t authority = value.find("//") + 2;
    const std::size_t path = value.find_first_of("/?#", authority);
    if (path != std::string::npos) value.resize(path);
    while (value.ends_with('/')) value.pop_back();
    return value;
}

}  // namespace remoe
