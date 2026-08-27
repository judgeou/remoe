#include "launcher_window.h"

#include "account_client.h"
#include "client_identity.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace remoe {
namespace {

constexpr int kServerEdit = 100;
constexpr int kLoginButton = 101;
constexpr int kHostList = 102;
constexpr int kRefreshButton = 103;
constexpr int kConnectButton = 104;
constexpr int kLogoutButton = 105;
constexpr int kFpsEdit = 106;
constexpr int kBitrateEdit = 107;
constexpr int kScaleEdit = 108;
constexpr int kStatusText = 109;
constexpr int kFixedQualityCheck = 110;
constexpr int kQualityEdit = 111;

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"Invalid UTF-8";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("invalid window text");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

std::uint32_t control_u32(HWND control, std::uint32_t minimum, std::uint32_t maximum,
                          const char* name) {
    const std::string text = narrow(control_text(control));
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end || value < minimum || value > maximum) {
        throw std::runtime_error(std::string(name) + " is out of range");
    }
    return static_cast<std::uint32_t>(value);
}

enum class TaskKind { None, Restore, StartAuthorization, PollAuthorization, RefreshHosts, Connect, Logout };

struct TaskResult {
    TaskKind kind = TaskKind::None;
    std::string error;
    std::optional<DeviceAuthorization> authorization;
    std::optional<NativeTokens> tokens;
    std::vector<AccountHost> hosts;
    std::string invite;
};

class LauncherWindow {
public:
    explicit LauncherWindow(std::function<void(const ClientLaunchSelection&)> start_session)
        : start_session_(std::move(start_session)) {}

    void show() {
        dpi_ = GetDpiForSystem();
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = L"RemoeClientLauncher";
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("could not register the client launcher window");
        }
        constexpr DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rectangle{0, 0, scale(620), scale(490)};
        AdjustWindowRectExForDpi(&rectangle, window_style, FALSE, 0, dpi_);
        window_ = CreateWindowExW(0, window_class.lpszClassName, L"remoe client", window_style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
                                  nullptr, nullptr,
                                  window_class.hInstance, this);
        if (!window_) {
            throw std::runtime_error("could not create the client launcher window (Win32 " +
                                     std::to_string(GetLastError()) + ")");
        }
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (task_.valid()) task_.wait();
        if (session_task_.valid()) session_task_.wait();
    }

private:
    int scale(int value) const noexcept {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<LauncherWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle_message(message, wparam, lparam)
                    : DefWindowProcW(window, message, wparam, lparam);
    }

    HWND add_control(const wchar_t* type, const wchar_t* text, DWORD style,
                     int x, int y, int width, int height, int id = 0) {
        HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                                       scale(x), scale(y), scale(width), scale(height), window_,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return control;
    }

    void create_font() {
        if (font_owned_ && font_) DeleteObject(font_);
        font_ = nullptr;
        font_owned_ = false;
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) {
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
            font_owned_ = font_ != nullptr;
        }
        if (!font_) font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    void create_controls() {
        create_font();
        add_control(L"STATIC", L"信令服务器", 0, 20, 20, 100, 24);
        server_ = add_control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 125, 18, 350, 25, kServerEdit);
        login_ = add_control(L"BUTTON", L"使用 passkey 登录", BS_PUSHBUTTON, 485, 17, 110, 27, kLoginButton);
        add_control(L"STATIC", L"我的电脑", 0, 20, 65, 100, 22);
        hosts_list_ = add_control(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                                  20, 90, 575, 245, kHostList);
        add_control(L"STATIC", L"FPS", 0, 20, 355, 35, 22);
        fps_ = add_control(L"EDIT", L"60", WS_BORDER | ES_NUMBER, 55, 352, 55, 25, kFpsEdit);
        add_control(L"STATIC", L"码率 Mbps", 0, 130, 355, 75, 22);
        bitrate_ = add_control(L"EDIT", L"20", WS_BORDER | ES_NUMBER, 210, 352, 65, 25, kBitrateEdit);
        add_control(L"STATIC", L"缩放 %", 0, 295, 355, 55, 22);
        scale_ = add_control(L"EDIT", L"100", WS_BORDER | ES_NUMBER, 355, 352, 60, 25, kScaleEdit);
        fixed_quality_ = add_control(L"BUTTON", L"固定质量", BS_AUTOCHECKBOX,
                                     430, 352, 80, 25, kFixedQualityCheck);
        quality_ = add_control(L"EDIT", L"28", WS_BORDER | ES_NUMBER,
                               520, 352, 55, 25, kQualityEdit);
        refresh_ = add_control(L"BUTTON", L"刷新", BS_PUSHBUTTON, 20, 395, 80, 28, kRefreshButton);
        connect_ = add_control(L"BUTTON", L"连接", BS_DEFPUSHBUTTON, 110, 395, 100, 28, kConnectButton);
        logout_ = add_control(L"BUTTON", L"退出登录", BS_PUSHBUTTON, 220, 395, 90, 28, kLogoutButton);
        status_ = add_control(L"STATIC", L"请输入服务器地址并使用 passkey 登录。", SS_LEFT,
                              20, 440, 575, 40, kStatusText);
        update_controls();
        if (const auto identity = load_client_identity()) {
            identity_ = identity;
            SetWindowTextW(server_, widen(identity->server_origin).c_str());
            begin_restore();
        }
    }

    void set_status(std::string_view value) { SetWindowTextW(status_, widen(value).c_str()); }

    void update_controls() {
        const BOOL idle = task_kind_ == TaskKind::None && !session_running_;
        EnableWindow(server_, idle && !identity_.has_value());
        EnableWindow(login_, idle && !identity_.has_value());
        EnableWindow(refresh_, idle && identity_.has_value());
        EnableWindow(logout_, idle && identity_.has_value());
        const int selected = static_cast<int>(SendMessageW(hosts_list_, LB_GETCURSEL, 0, 0));
        const bool host_online = selected >= 0 && static_cast<std::size_t>(selected) < hosts_.size() &&
                                 hosts_[static_cast<std::size_t>(selected)].online;
        EnableWindow(connect_, idle && identity_.has_value() && host_online);
        const bool fixed_quality =
            SendMessageW(fixed_quality_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        EnableWindow(bitrate_, idle && !fixed_quality);
        EnableWindow(quality_, idle && fixed_quality);
    }

    template <typename Function>
    void begin_task(TaskKind kind, Function function) {
        if (task_kind_ != TaskKind::None) return;
        if (task_.valid()) task_.get();
        task_kind_ = kind;
        update_controls();
        task_ = std::async(std::launch::async, [kind, function = std::move(function)]() mutable {
            try {
                TaskResult result = function();
                result.kind = kind;
                return result;
            } catch (const std::exception& error) {
                return TaskResult{.kind = kind, .error = error.what()};
            }
        });
    }

    void begin_restore() {
        const ClientIdentity identity = *identity_;
        set_status("正在恢复登录…");
        begin_task(TaskKind::Restore, [identity] {
            AccountClient account(identity.server_origin);
            TaskResult result;
            result.tokens = account.refresh(identity.refresh_token);
            result.hosts = account.hosts(result.tokens->access_token);
            return result;
        });
    }

    void begin_login() {
        const std::string server = narrow(control_text(server_));
        set_status("正在创建浏览器授权请求…");
        begin_task(TaskKind::StartAuthorization, [server] {
            AccountClient account(server);
            TaskResult result;
            result.authorization = account.start_device_authorization("remoe Windows client");
            return result;
        });
    }

    void begin_poll() {
        const std::string server = server_origin_;
        const DeviceAuthorization authorization = *authorization_;
        begin_task(TaskKind::PollAuthorization, [server, authorization] {
            AccountClient account(server);
            TaskResult result;
            const auto polled = account.poll_device_authorization(authorization);
            if (!polled.pending) {
                result.tokens = polled.tokens;
                result.hosts = account.hosts(polled.tokens.access_token);
            }
            return result;
        });
    }

    void begin_refresh() {
        const ClientIdentity identity = *identity_;
        set_status("正在刷新电脑列表…");
        begin_task(TaskKind::RefreshHosts, [identity] {
            AccountClient account(identity.server_origin);
            TaskResult result;
            result.tokens = account.refresh(identity.refresh_token);
            result.hosts = account.hosts(result.tokens->access_token);
            return result;
        });
    }

    void begin_connect() {
        const int selected = static_cast<int>(SendMessageW(hosts_list_, LB_GETCURSEL, 0, 0));
        if (selected < 0 || static_cast<std::size_t>(selected) >= hosts_.size()) return;
        try {
            pending_selection_.fps = control_u32(fps_, 1, 240, "FPS");
            pending_selection_.bitrate_mbps = control_u32(bitrate_, 1, 1000, "bitrate");
            pending_selection_.scale_percent = control_u32(scale_, 10, 100, "scale");
            pending_selection_.fixed_quality =
                SendMessageW(fixed_quality_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            pending_selection_.quality = control_u32(quality_, 1, 51, "quality");
        } catch (const std::exception& error) {
            set_status(error.what());
            return;
        }
        const ClientIdentity identity = *identity_;
        const std::string host_id = hosts_[static_cast<std::size_t>(selected)].id;
        set_status("正在创建安全连接…");
        begin_task(TaskKind::Connect, [identity, host_id] {
            AccountClient account(identity.server_origin);
            TaskResult result;
            result.tokens = account.refresh(identity.refresh_token);
            result.invite = account.connect_host(host_id, result.tokens->access_token);
            return result;
        });
    }

    void begin_logout() {
        const ClientIdentity identity = *identity_;
        set_status("正在退出登录…");
        begin_task(TaskKind::Logout, [identity] {
            AccountClient(identity.server_origin).logout(identity.refresh_token);
            return TaskResult{};
        });
    }

    void replace_hosts(std::vector<AccountHost> hosts) {
        hosts_ = std::move(hosts);
        SendMessageW(hosts_list_, LB_RESETCONTENT, 0, 0);
        for (const auto& host : hosts_) {
            const std::wstring label = widen(host.name) + (host.online ? L"  · 在线" : L"  · 离线");
            SendMessageW(hosts_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!hosts_.empty()) SendMessageW(hosts_list_, LB_SETCURSEL, 0, 0);
    }

    void start_remote_session(std::string invite) {
        pending_selection_.invite_url = std::move(invite);
        const ClientLaunchSelection selection = pending_selection_;
        session_running_ = true;
        set_status("远程会话已在最大化窗口中启动；关闭播放窗口即可断开。");
        update_controls();
        session_task_ = std::async(std::launch::async,
            [callback = start_session_, selection] {
                try {
                    callback(selection);
                    return std::string{};
                } catch (const std::exception& error) {
                    return std::string(error.what());
                }
            });
    }

    void finish_remote_session() {
        const std::string error = session_task_.get();
        session_running_ = false;
        set_status(error.empty() ? "远程会话已断开，可以重新连接。"
                                 : "远程会话失败：" + error);
        update_controls();
    }

    void finish_task() {
        TaskResult result = task_.get();
        task_kind_ = TaskKind::None;
        if (!result.error.empty()) {
            if (result.kind == TaskKind::Restore) {
                delete_client_identity();
                identity_.reset();
                access_token_.clear();
                replace_hosts({});
                set_status("登录已失效，请重新使用 passkey 登录：" + result.error);
            } else {
                if (result.kind == TaskKind::PollAuthorization) authorization_.reset();
                set_status(result.error);
            }
            update_controls();
            return;
        }
        if (result.kind == TaskKind::StartAuthorization) {
            authorization_ = std::move(result.authorization);
            server_origin_ = AccountClient(narrow(control_text(server_))).server_origin();
            SetWindowTextW(server_, widen(server_origin_).c_str());
            const auto opened = ShellExecuteW(window_, L"open",
                widen(authorization_->verification_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(opened) <= 32) {
                authorization_.reset();
                set_status("无法打开系统浏览器，请检查 Windows 默认浏览器设置。");
                update_controls();
                return;
            }
            next_poll_ = std::chrono::steady_clock::now();
            set_status("请在浏览器中使用 passkey 登录并允许此 Client。授权码：" +
                       authorization_->user_code);
        } else if (result.kind == TaskKind::PollAuthorization) {
            if (result.tokens) {
                access_token_ = result.tokens->access_token;
                identity_ = ClientIdentity{server_origin_, result.tokens->refresh_token};
                save_client_identity(*identity_);
                authorization_.reset();
                replace_hosts(std::move(result.hosts));
                set_status("登录成功。请选择一台在线电脑。");
            } else {
                next_poll_ = std::chrono::steady_clock::now() +
                    std::chrono::seconds(authorization_->poll_interval);
            }
        } else if (result.kind == TaskKind::Restore || result.kind == TaskKind::RefreshHosts) {
            access_token_ = result.tokens->access_token;
            server_origin_ = identity_->server_origin;
            replace_hosts(std::move(result.hosts));
            set_status("已登录。请选择一台在线电脑。");
        } else if (result.kind == TaskKind::Connect) {
            access_token_ = result.tokens->access_token;
            start_remote_session(std::move(result.invite));
        } else if (result.kind == TaskKind::Logout) {
            delete_client_identity();
            identity_.reset();
            access_token_.clear();
            server_origin_.clear();
            replace_hosts({});
            set_status("已退出登录。");
        }
        update_controls();
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            create_controls();
            SetTimer(window_, 1, 200, nullptr);
            return 0;
        case WM_DPICHANGED: {
            const UINT previous_dpi = dpi_;
            dpi_ = HIWORD(wparam);
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            struct ScaleContext { HWND parent; UINT from; UINT to; } context{
                window_, previous_dpi, dpi_};
            EnumChildWindows(window_, [](HWND child, LPARAM parameter) -> BOOL {
                const auto& scale_context = *reinterpret_cast<ScaleContext*>(parameter);
                RECT rectangle{};
                GetWindowRect(child, &rectangle);
                MapWindowPoints(HWND_DESKTOP, scale_context.parent,
                                reinterpret_cast<POINT*>(&rectangle), 2);
                SetWindowPos(child, nullptr,
                    MulDiv(rectangle.left, scale_context.to, scale_context.from),
                    MulDiv(rectangle.top, scale_context.to, scale_context.from),
                    MulDiv(rectangle.right - rectangle.left, scale_context.to, scale_context.from),
                    MulDiv(rectangle.bottom - rectangle.top, scale_context.to, scale_context.from),
                    SWP_NOACTIVATE | SWP_NOZORDER);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&context));
            create_font();
            EnumChildWindows(window_, [](HWND child, LPARAM font) -> BOOL {
                SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
                return TRUE;
            }, reinterpret_cast<LPARAM>(font_));
            return 0;
        }
        case WM_TIMER:
            if (task_kind_ != TaskKind::None && task_.valid() &&
                task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                finish_task();
            } else if (session_running_ && session_task_.valid() &&
                       session_task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                finish_remote_session();
            } else if (task_kind_ == TaskKind::None && authorization_ &&
                       std::chrono::steady_clock::now() >= next_poll_) {
                begin_poll();
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == LBN_SELCHANGE && LOWORD(wparam) == kHostList) update_controls();
            else if (HIWORD(wparam) == BN_CLICKED) {
                switch (LOWORD(wparam)) {
                case kLoginButton: begin_login(); break;
                case kRefreshButton: begin_refresh(); break;
                case kConnectButton: begin_connect(); break;
                case kLogoutButton: begin_logout(); break;
                case kFixedQualityCheck: update_controls(); break;
                }
            }
            return 0;
        case WM_CLOSE:
            if (session_running_) {
                set_status("请先关闭远程播放窗口，再退出 remoe client。");
                return 0;
            }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, 1);
            if (font_owned_ && font_) DeleteObject(font_);
            font_ = nullptr;
            font_owned_ = false;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
        }
    }

    HWND window_ = nullptr;
    HWND server_ = nullptr;
    HWND login_ = nullptr;
    HWND hosts_list_ = nullptr;
    HWND refresh_ = nullptr;
    HWND connect_ = nullptr;
    HWND logout_ = nullptr;
    HWND fps_ = nullptr;
    HWND bitrate_ = nullptr;
    HWND scale_ = nullptr;
    HWND fixed_quality_ = nullptr;
    HWND quality_ = nullptr;
    HWND status_ = nullptr;
    HFONT font_ = nullptr;
    bool font_owned_ = false;
    UINT dpi_ = 96;
    TaskKind task_kind_ = TaskKind::None;
    std::future<TaskResult> task_;
    std::future<std::string> session_task_;
    std::function<void(const ClientLaunchSelection&)> start_session_;
    bool session_running_ = false;
    std::optional<ClientIdentity> identity_;
    std::optional<DeviceAuthorization> authorization_;
    std::chrono::steady_clock::time_point next_poll_{};
    std::string server_origin_;
    std::string access_token_;
    std::vector<AccountHost> hosts_;
    ClientLaunchSelection pending_selection_;
};

}  // namespace

void show_client_launcher(std::function<void(const ClientLaunchSelection&)> start_session) {
    LauncherWindow(std::move(start_session)).show();
}

}  // namespace remoe
