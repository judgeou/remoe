#include "windows_service.h"

#include <Windows.h>
#include <ShlObj.h>
#include <UserEnv.h>
#include <WtsApi32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace remoe {
namespace {

#if defined(REMOE_X264_HOST)
constexpr wchar_t kServiceName[] = L"remoe-host-x264";
constexpr wchar_t kServiceDisplayName[] = L"remoe Host Service (x264)";
#else
constexpr wchar_t kServiceName[] = L"remoe-host";
constexpr wchar_t kServiceDisplayName[] = L"remoe Host Service";
#endif

class Win32Error : public std::runtime_error {
public:
    Win32Error(std::string operation, DWORD code)
        : std::runtime_error(std::move(operation) + " failed, Win32 error " +
                             std::to_string(code)), code_(code) {}

    [[nodiscard]] DWORD code() const noexcept { return code_; }

private:
    DWORD code_;
};

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

class UniqueServiceHandle {
public:
    UniqueServiceHandle() = default;
    explicit UniqueServiceHandle(SC_HANDLE value) : value_(value) {}
    ~UniqueServiceHandle() { reset(); }
    UniqueServiceHandle(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;
    UniqueServiceHandle(UniqueServiceHandle&& other) noexcept : value_(other.release()) {}
    UniqueServiceHandle& operator=(UniqueServiceHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    [[nodiscard]] SC_HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    SC_HANDLE release() noexcept {
        SC_HANDLE value = value_;
        value_ = nullptr;
        return value;
    }
    void reset(SC_HANDLE value = nullptr) noexcept {
        if (value_) CloseServiceHandle(value_);
        value_ = value;
    }

private:
    SC_HANDLE value_ = nullptr;
};

[[noreturn]] void throw_last_error(const char* operation) {
    throw Win32Error(operation, GetLastError());
}

std::wstring current_executable_path() {
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size == path.size()) throw_last_error("GetModuleFileNameW");
    return std::wstring(path.data(), size);
}

std::filesystem::path service_install_directory() {
    PWSTR program_files = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &program_files);
    if (FAILED(result)) {
        throw std::runtime_error("SHGetKnownFolderPath(ProgramFiles) failed, HRESULT " +
                                 std::to_string(static_cast<unsigned long>(result)));
    }
    const std::filesystem::path path = std::filesystem::path(program_files) / L"remoe";
    CoTaskMemFree(program_files);
    return path;
}

std::filesystem::path stage_service_files() {
    const std::filesystem::path source_executable = current_executable_path();
    const std::filesystem::path source_directory = source_executable.parent_path();
    const std::filesystem::path install_directory = service_install_directory();
    std::filesystem::create_directories(install_directory);

    const std::filesystem::path installed_executable =
        install_directory / source_executable.filename();
    std::error_code equivalent_error;
    const bool executable_is_installed = std::filesystem::exists(installed_executable) &&
        std::filesystem::equivalent(source_executable, installed_executable, equivalent_error) &&
        !equivalent_error;
    if (!executable_is_installed) {
        std::filesystem::copy_file(source_executable, installed_executable,
                                   std::filesystem::copy_options::overwrite_existing);
    }

    const std::filesystem::path source_datachannel = source_directory / L"datachannel.dll";
    if (!std::filesystem::exists(source_datachannel)) {
        throw std::runtime_error("datachannel.dll is missing beside the Host executable");
    }
    const std::filesystem::path installed_datachannel = install_directory / L"datachannel.dll";
    equivalent_error.clear();
    const bool datachannel_is_installed = std::filesystem::exists(installed_datachannel) &&
        std::filesystem::equivalent(source_datachannel, installed_datachannel,
                                    equivalent_error) &&
        !equivalent_error;
    if (!datachannel_is_installed) {
        std::filesystem::copy_file(source_datachannel, installed_datachannel,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    return installed_executable;
}

std::wstring quote_windows_argument(std::wstring_view argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }

    std::wstring quoted(1, L'\"');
    std::size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::wstring make_command_line(std::wstring_view executable,
                               std::wstring_view internal_argument,
                               const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_windows_argument(executable);
    if (!internal_argument.empty()) {
        command.push_back(L' ');
        command += internal_argument;
    }
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_windows_argument(argument);
    }
    return command;
}

SERVICE_STATUS_PROCESS query_service_status(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes)) {
        throw_last_error("QueryServiceStatusEx");
    }
    return status;
}

SERVICE_STATUS_PROCESS wait_for_service_state(SC_HANDLE service, DWORD target,
                                               std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto status = query_service_status(service);
        if (status.dwCurrentState == target) return status;
        if (target == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) return status;
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for the remoe Host service");
        }
        Sleep(100);
    }
}

void stop_service_if_running(SC_HANDLE service) {
    auto status = query_service_status(service);
    if (status.dwCurrentState == SERVICE_STOPPED) return;

    SERVICE_STATUS ignored{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) throw Win32Error("ControlService", error);
    }
    wait_for_service_state(service, SERVICE_STOPPED, std::chrono::seconds(20));
}

void enable_privilege(const wchar_t* name) {
    UniqueHandle token;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &raw_token)) {
        throw_last_error("OpenProcessToken");
    }
    token.reset(raw_token);

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid)) {
        throw_last_error("LookupPrivilegeValueW");
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr,
                               nullptr)) {
        throw_last_error("AdjustTokenPrivileges");
    }
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        throw Win32Error("AdjustTokenPrivileges", ERROR_NOT_ALL_ASSIGNED);
    }
}

std::optional<DWORD> active_interactive_session() {
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        throw_last_error("WTSEnumerateSessionsW");
    }

    const DWORD console_session = WTSGetActiveConsoleSessionId();
    std::optional<DWORD> fallback;
    for (DWORD index = 0; index < count; ++index) {
        const auto& session = sessions[index];
        if (session.SessionId == 0 || session.State != WTSActive) continue;
        if (session.SessionId == console_session) {
            WTSFreeMemory(sessions);
            return session.SessionId;
        }
        if (!fallback) fallback = session.SessionId;
    }
    WTSFreeMemory(sessions);
    return fallback;
}

struct WorkerProcess {
    UniqueHandle process;
    UniqueHandle stop_event;
    DWORD session_id = 0;
};

std::uint64_t g_worker_sequence = 0;

WorkerProcess launch_worker(DWORD session_id,
                            const std::vector<std::wstring>& worker_arguments) {
    enable_privilege(L"SeTcbPrivilege");

    UniqueHandle process_token;
    HANDLE raw_process_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                          &raw_process_token)) {
        throw_last_error("OpenProcessToken");
    }
    process_token.reset(raw_process_token);

    UniqueHandle worker_token;
    HANDLE raw_worker_token = nullptr;
    if (!DuplicateTokenEx(process_token.get(), MAXIMUM_ALLOWED, nullptr,
                          SecurityImpersonation, TokenPrimary, &raw_worker_token)) {
        throw_last_error("DuplicateTokenEx");
    }
    worker_token.reset(raw_worker_token);
    if (!SetTokenInformation(worker_token.get(), TokenSessionId, &session_id,
                             sizeof(session_id))) {
        throw_last_error("SetTokenInformation(TokenSessionId)");
    }

    const std::wstring stop_event_name =
        L"Global\\remoe-host-worker-stop-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(++g_worker_sequence);
    UniqueHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, stop_event_name.c_str()));
    if (!stop_event) throw_last_error("CreateEventW");

    std::vector<std::wstring> child_arguments = worker_arguments;
    child_arguments.emplace_back(L"--service-stop-event");
    child_arguments.push_back(stop_event_name);

    const std::wstring executable = current_executable_path();
    std::wstring command = make_command_line(executable, L"--system-worker", child_arguments);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    void* environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, worker_token.get(), FALSE)) {
        throw_last_error("CreateEnvironmentBlock");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = std::filesystem::path(executable).parent_path().wstring();
    const BOOL created = CreateProcessAsUserW(
        worker_token.get(), executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, environment,
        working_directory.c_str(), &startup, &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    DestroyEnvironmentBlock(environment);
    if (!created) throw Win32Error("CreateProcessAsUserW", create_error);

    CloseHandle(process.hThread);
    return {UniqueHandle(process.hProcess), std::move(stop_event), session_id};
}

void stop_worker(WorkerProcess& worker) {
    if (!worker.process) return;
    SetEvent(worker.stop_event.get());
    if (WaitForSingleObject(worker.process.get(), 5000) == WAIT_TIMEOUT) {
        TerminateProcess(worker.process.get(), ERROR_PROCESS_ABORTED);
        WaitForSingleObject(worker.process.get(), 2000);
    }
    worker.process.reset();
    worker.stop_event.reset();
    worker.session_id = 0;
}

SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};
std::mutex g_service_status_mutex;
UniqueHandle g_service_stop_event;
UniqueHandle g_service_session_event;
std::vector<std::wstring> g_service_worker_arguments;

void report_service_status(DWORD state, DWORD error = ERROR_SUCCESS,
                           DWORD wait_hint = 0) noexcept {
    std::lock_guard lock(g_service_status_mutex);
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE
        : 0;
    g_service_status.dwWin32ExitCode = error;
    g_service_status.dwServiceSpecificExitCode = 0;
    g_service_status.dwCheckPoint =
        state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING
            ? g_service_status.dwCheckPoint + 1
            : 0;
    g_service_status.dwWaitHint = wait_hint;
    if (g_service_status_handle) {
        SetServiceStatus(g_service_status_handle, &g_service_status);
    }
}

DWORD WINAPI service_control_handler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 7000);
        if (g_service_stop_event) SetEvent(g_service_stop_event.get());
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_SESSIONCHANGE) {
        if (g_service_session_event) SetEvent(g_service_session_event.get());
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) {
        std::lock_guard lock(g_service_status_mutex);
        if (g_service_status_handle) {
            SetServiceStatus(g_service_status_handle, &g_service_status);
        }
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, wchar_t**) {
    DWORD exit_error = ERROR_SUCCESS;
    WorkerProcess worker;
    try {
        g_service_status_handle = RegisterServiceCtrlHandlerExW(
            kServiceName, service_control_handler, nullptr);
        if (!g_service_status_handle) throw_last_error("RegisterServiceCtrlHandlerExW");
        report_service_status(SERVICE_START_PENDING, ERROR_SUCCESS, 10000);

        g_service_stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!g_service_stop_event) throw_last_error("CreateEventW(stop)");
        g_service_session_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!g_service_session_event) throw_last_error("CreateEventW(session)");

        if (auto session = active_interactive_session()) {
            worker = launch_worker(*session, g_service_worker_arguments);
        }
        report_service_status(SERVICE_RUNNING);

        for (;;) {
            HANDLE waits[3] = {g_service_stop_event.get(), g_service_session_event.get(),
                               worker.process.get()};
            const DWORD count = worker.process ? 3u : 2u;
            const DWORD result = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) break;

            if (result == WAIT_OBJECT_0 + 1) {
                const auto session = active_interactive_session();
                if (worker.process && (!session || *session != worker.session_id)) {
                    stop_worker(worker);
                }
                if (!worker.process && session) {
                    worker = launch_worker(*session, g_service_worker_arguments);
                }
                continue;
            }

            if (worker.process && result == WAIT_OBJECT_0 + 2) {
                worker.process.reset();
                worker.stop_event.reset();
                worker.session_id = 0;
                if (WaitForSingleObject(g_service_stop_event.get(), 2000) == WAIT_OBJECT_0) break;
                if (auto session = active_interactive_session()) {
                    worker = launch_worker(*session, g_service_worker_arguments);
                }
                continue;
            }
            throw Win32Error("WaitForMultipleObjects", GetLastError());
        }
        report_service_status(SERVICE_STOP_PENDING, ERROR_SUCCESS, 7000);
        stop_worker(worker);
    } catch (const Win32Error& error) {
        exit_error = error.code();
    } catch (...) {
        exit_error = ERROR_EXCEPTION_IN_SERVICE;
    }

    stop_worker(worker);
    g_service_session_event.reset();
    g_service_stop_event.reset();
    report_service_status(SERVICE_STOPPED, exit_error);
}

} // namespace

bool is_running_as_local_system() {
    BYTE sid_buffer[SECURITY_MAX_SID_SIZE]{};
    DWORD sid_size = sizeof(sid_buffer);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sid_buffer, &sid_size)) {
        throw_last_error("CreateWellKnownSid");
    }
    BOOL is_member = FALSE;
    if (!CheckTokenMembership(nullptr, sid_buffer, &is_member)) {
        throw_last_error("CheckTokenMembership");
    }
    return is_member != FALSE;
}

int install_host_service(const std::vector<std::wstring>& worker_arguments) {
    UniqueServiceHandle manager(OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!manager) throw_last_error("OpenSCManagerW");

    constexpr DWORD access = SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS |
                             SERVICE_START | SERVICE_STOP | DELETE;
    UniqueServiceHandle service(OpenServiceW(manager.get(), kServiceName, access));
    if (!service) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) {
            throw Win32Error("OpenServiceW", error);
        }
    } else {
        stop_service_if_running(service.get());
    }

    // Never point a LocalSystem service at a developer build directory. Files
    // staged under Program Files inherit an administrator-controlled ACL and
    // cannot be replaced by an unelevated process.
    const std::wstring installed_executable = stage_service_files().wstring();
    const std::wstring binary_path =
        make_command_line(installed_executable, L"--service", worker_arguments);

    if (!service) {
        service.reset(CreateServiceW(
            manager.get(), kServiceName, kServiceDisplayName, access,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binary_path.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
        if (!service) throw_last_error("CreateServiceW");
    } else {
        if (!ChangeServiceConfigW(
                service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL, binary_path.c_str(), nullptr, nullptr, nullptr, nullptr,
                nullptr, kServiceDisplayName)) {
            throw_last_error("ChangeServiceConfigW");
        }
    }

    SERVICE_DESCRIPTIONW description{};
    description.lpDescription = const_cast<wchar_t*>(
        L"Runs the remoe Host as LocalSystem in the active interactive Windows session.");
    if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description)) {
        throw_last_error("ChangeServiceConfig2W(description)");
    }
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
    if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) {
        throw_last_error("ChangeServiceConfig2W(delayed start)");
    }

    if (!StartServiceW(service.get(), 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) throw Win32Error("StartServiceW", error);
    }
    const auto status = wait_for_service_state(
        service.get(), SERVICE_RUNNING, std::chrono::seconds(20));
    if (status.dwCurrentState != SERVICE_RUNNING) {
        throw Win32Error("Starting remoe Host service",
                         status.dwWin32ExitCode == ERROR_SUCCESS
                             ? ERROR_SERVICE_NOT_ACTIVE
                             : status.dwWin32ExitCode);
    }

    std::wcout << kServiceDisplayName
               << L" installed and started as LocalSystem (automatic delayed start).\n";
    return 0;
}

int uninstall_host_service() {
    UniqueServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) throw_last_error("OpenSCManagerW");

    UniqueServiceHandle service(OpenServiceW(
        manager.get(), kServiceName, SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
    if (!service) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::wcout << kServiceDisplayName << L" is not installed.\n";
            return 0;
        }
        throw Win32Error("OpenServiceW", error);
    }

    stop_service_if_running(service.get());
    if (!DeleteService(service.get())) throw_last_error("DeleteService");
    std::wcout << kServiceDisplayName << L" stopped and removed.\n";
    return 0;
}

int run_host_service(const std::vector<std::wstring>& worker_arguments) {
    g_service_worker_arguments = worker_arguments;
    SERVICE_TABLE_ENTRYW dispatch_table[] = {
        {const_cast<wchar_t*>(kServiceName), service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(dispatch_table)) {
        throw_last_error("StartServiceCtrlDispatcherW");
    }
    return 0;
}

} // namespace remoe
