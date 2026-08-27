#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace remoe {

struct DeviceAuthorization {
    std::string request_id;
    std::string device_secret;
    std::string user_code;
    std::string verification_url;
    std::uint32_t poll_interval = 3;
};

struct NativeTokens {
    std::string access_token;
    std::string refresh_token;
    std::uint32_t expires_in = 0;
};

struct PollAuthorizationResult {
    bool pending = false;
    NativeTokens tokens;
};

struct AccountHost {
    std::string id;
    std::string name;
    bool online = false;
};

class AccountClient {
public:
    explicit AccountClient(std::string server_origin);

    const std::string& server_origin() const noexcept { return server_origin_; }
    DeviceAuthorization start_device_authorization(std::string_view client_name) const;
    PollAuthorizationResult poll_device_authorization(const DeviceAuthorization& authorization) const;
    NativeTokens refresh(std::string_view refresh_token) const;
    std::vector<AccountHost> hosts(std::string_view access_token) const;
    std::string connect_host(std::string_view host_id, std::string_view access_token) const;
    void logout(std::string_view refresh_token) const;

private:
    std::string server_origin_;
};

}  // namespace remoe
