#include "account_client.h"

#include "http_client.h"

#include <charconv>
#include <optional>
#include <stdexcept>

namespace remoe {
namespace {

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                result += "\\u00";
                result.push_back(hex[c >> 4]);
                result.push_back(hex[c & 0xf]);
            } else result.push_back(static_cast<char>(c));
        }
    }
    result.push_back('"');
    return result;
}

std::size_t value_position(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const std::size_t found = json.find(needle);
    if (found == std::string_view::npos) throw std::runtime_error("server response is missing " + std::string(key));
    const std::size_t colon = json.find(':', found + needle.size());
    if (colon == std::string_view::npos) throw std::runtime_error("server returned invalid JSON");
    const std::size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string_view::npos) throw std::runtime_error("server returned invalid JSON");
    return value;
}

void append_utf8(std::string& output, unsigned codepoint) {
    if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

unsigned hex4(std::string_view json, std::size_t offset) {
    if (offset + 4 > json.size()) throw std::runtime_error("server returned invalid JSON escape");
    unsigned value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = json[offset + i];
        value <<= 4;
        if (c >= '0' && c <= '9') value += static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') value += static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value += static_cast<unsigned>(c - 'A' + 10);
        else throw std::runtime_error("server returned invalid JSON escape");
    }
    return value;
}

std::string json_string(std::string_view json, std::string_view key) {
    std::size_t position = value_position(json, key);
    if (json[position++] != '"') throw std::runtime_error("server returned an invalid " + std::string(key));
    std::string result;
    while (position < json.size()) {
        const char c = json[position++];
        if (c == '"') return result;
        if (c != '\\') { result.push_back(c); continue; }
        if (position >= json.size()) break;
        const char escape = json[position++];
        switch (escape) {
        case '"': case '\\': case '/': result.push_back(escape); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': append_utf8(result, hex4(json, position)); position += 4; break;
        default: throw std::runtime_error("server returned invalid JSON escape");
        }
    }
    throw std::runtime_error("server returned unterminated JSON text");
}

std::uint32_t json_u32(std::string_view json, std::string_view key) {
    const std::size_t position = value_position(json, key);
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(json.data() + position, json.data() + json.size(), result);
    if (parsed.ec != std::errc{}) throw std::runtime_error("server returned an invalid " + std::string(key));
    return result;
}

bool json_bool(std::string_view json, std::string_view key) {
    const std::size_t position = value_position(json, key);
    if (json.substr(position, 4) == "true") return true;
    if (json.substr(position, 5) == "false") return false;
    throw std::runtime_error("server returned an invalid " + std::string(key));
}

std::string error_message(const HttpResponse& response) {
    try { return json_string(response.body, "error"); }
    catch (...) { return "HTTP request failed with status " + std::to_string(response.status); }
}

void require_success(const HttpResponse& response) {
    if (response.status < 200 || response.status >= 300) throw std::runtime_error(error_message(response));
}

std::vector<std::string_view> json_array_objects(std::string_view json, std::string_view key) {
    std::size_t position = value_position(json, key);
    if (json[position++] != '[') throw std::runtime_error("server returned an invalid array");
    std::vector<std::string_view> result;
    while (position < json.size()) {
        position = json.find_first_not_of(" \t\r\n,", position);
        if (position == std::string_view::npos || json[position] == ']') return result;
        if (json[position] != '{') throw std::runtime_error("server returned an invalid array item");
        const std::size_t begin = position;
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (; position < json.size(); ++position) {
            const char c = json[position];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                result.push_back(json.substr(begin, position - begin + 1));
                ++position;
                break;
            }
        }
        if (depth != 0) throw std::runtime_error("server returned an unterminated object");
    }
    throw std::runtime_error("server returned an unterminated array");
}

}  // namespace

AccountClient::AccountClient(std::string server_origin)
    : server_origin_(normalize_server_origin(std::move(server_origin))) {}

DeviceAuthorization AccountClient::start_device_authorization(std::string_view client_name) const {
    const auto response = http_request(server_origin_ + "/api/client/device/start", L"POST",
        "{\"clientName\":" + json_escape(client_name) + "}");
    require_success(response);
    return {
        json_string(response.body, "requestId"), json_string(response.body, "deviceSecret"),
        json_string(response.body, "userCode"), json_string(response.body, "verificationUrl"),
        json_u32(response.body, "pollInterval"),
    };
}

PollAuthorizationResult AccountClient::poll_device_authorization(
    const DeviceAuthorization& authorization) const {
    const std::string body = "{\"requestId\":" + json_escape(authorization.request_id) +
        ",\"deviceSecret\":" + json_escape(authorization.device_secret) + "}";
    const auto response = http_request(server_origin_ + "/api/client/device/poll", L"POST", body);
    if (response.status == 202) return {.pending = true};
    require_success(response);
    return {.pending = false, .tokens = {
        json_string(response.body, "accessToken"), json_string(response.body, "refreshToken"),
        json_u32(response.body, "expiresIn"),
    }};
}

NativeTokens AccountClient::refresh(std::string_view refresh_token) const {
    const auto response = http_request(server_origin_ + "/api/client/token/refresh", L"POST",
        "{\"refreshToken\":" + json_escape(refresh_token) + "}");
    require_success(response);
    return {json_string(response.body, "accessToken"), std::string(refresh_token),
            json_u32(response.body, "expiresIn")};
}

std::vector<AccountHost> AccountClient::hosts(std::string_view access_token) const {
    const auto response = http_request(server_origin_ + "/api/client/hosts", L"GET", {}, access_token);
    require_success(response);
    std::vector<AccountHost> result;
    for (const auto object : json_array_objects(response.body, "hosts")) {
        result.push_back({json_string(object, "id"), json_string(object, "name"),
                          json_bool(object, "online")});
    }
    return result;
}

std::string AccountClient::connect_host(std::string_view host_id,
                                        std::string_view access_token) const {
    const auto response = http_request(server_origin_ + "/api/client/hosts/" +
        std::string(host_id) + "/connect", L"POST", "{}", access_token);
    require_success(response);
    return json_string(response.body, "invite");
}

void AccountClient::logout(std::string_view refresh_token) const {
    const auto response = http_request(server_origin_ + "/api/client/logout", L"POST",
        "{\"refreshToken\":" + json_escape(refresh_token) + "}");
    require_success(response);
}

}  // namespace remoe
