#pragma once

#include <string>
#include <vector>

namespace remoe {

[[nodiscard]] bool is_running_as_local_system();

// Installs an automatically started LocalSystem broker. The broker launches the
// actual Host worker in the active interactive Windows session.
int install_host_service(const std::vector<std::wstring>& worker_arguments);
int uninstall_host_service();

// Called only by the Service Control Manager through the internal --service
// command-line entry point.
int run_host_service(const std::vector<std::wstring>& worker_arguments);

} // namespace remoe
