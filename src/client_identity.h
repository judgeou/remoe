#pragma once

#include <optional>
#include <string>

namespace remoe {

struct ClientIdentity {
    std::string server_origin;
    std::string refresh_token;
};

std::optional<ClientIdentity> load_client_identity();
void save_client_identity(const ClientIdentity& identity);
void delete_client_identity() noexcept;

}  // namespace remoe
