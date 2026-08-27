#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace remoe {

struct ClientLaunchSelection {
    std::string invite_url;
    std::uint32_t fps = 60;
    std::uint32_t bitrate_mbps = 20;
    std::uint32_t scale_percent = 100;
};

void show_client_launcher(std::function<void(const ClientLaunchSelection&)> start_session);

}  // namespace remoe
