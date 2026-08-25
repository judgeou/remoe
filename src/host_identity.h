#pragma once

#include "webrtc_websocket_signaling.h"

#include <optional>

namespace remoe {

std::optional<ManagedHostIdentity> load_host_identity();
void save_host_identity(const ManagedHostIdentity& identity);

} // namespace remoe
