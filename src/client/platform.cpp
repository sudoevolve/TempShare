#include "platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#elif defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#else
#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

std::optional<bool> system_dark_mode() {
#ifdef _WIN32
    DWORD light = 0;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS) {
        return light == 0;
    }
#elif defined(__APPLE__)
    @autoreleasepool {
        NSAppearance* appearance = NSApp.effectiveAppearance;
        if (appearance) {
            return [[appearance bestMatchFromAppearancesWithNames:
                @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]] isEqualToString:NSAppearanceNameDarkAqua];
        }
    }
#else
    if (const auto* theme = std::getenv("GTK_THEME")) {
        return std::string_view(theme).find("dark") != std::string_view::npos;
    }
    if (auto* pipe = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r")) {
        char value[64]{};
        const bool read = std::fgets(value, sizeof(value), pipe) != nullptr;
        const int status = pclose(pipe);
        if (read && status == 0) {
            if (std::string_view(value).find("prefer-dark") != std::string_view::npos) return true;
            return false;
        }
    }
#endif
    return std::nullopt;
}

std::vector<std::filesystem::path> choose_paths(bool directory, bool* failed) {
    if (failed) *failed = false;
    std::vector<std::filesystem::path> paths;
#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        if (failed) *failed = true;
        return paths;
    }
    {
        ComPtr<IFileOpenDialog> dialog;
        const auto setup = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(dialog.GetAddressOf()));
        if (SUCCEEDED(setup)) {
            FILEOPENDIALOGOPTIONS options{};
            auto result = dialog->GetOptions(&options);
            if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
                (directory ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT));
            if (SUCCEEDED(result)) result = dialog->Show(GetActiveWindow());
            if (SUCCEEDED(result)) {
                ComPtr<IShellItemArray> items;
                DWORD count = 0;
                if (SUCCEEDED(dialog->GetResults(items.GetAddressOf()))) items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    ComPtr<IShellItem> item;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, item.GetAddressOf())) &&
                        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        paths.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                }
            }
            if (failed && result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) *failed = paths.empty();
        } else if (failed) {
            *failed = true;
        }
    }
    CoUninitialize();
#elif defined(__APPLE__)
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = directory;
        panel.canChooseFiles = !directory;
        panel.allowsMultipleSelection = !directory;
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL* url in panel.URLs) paths.emplace_back(url.fileSystemRepresentation);
        }
    }
#else
    const char* command = directory ?
        "zenity --file-selection --directory 2>/dev/null" :
        "zenity --file-selection --multiple --separator='|' 2>/dev/null";
    if (auto* pipe = popen(command, "r")) {
        char buffer[4096];
        std::string result;
        while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
        const int status = pclose(pipe);
        if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) > 1) {
            if (failed) *failed = true;
            return paths;
        }
        if (status || result.empty()) return paths;
        if (result.back() == '\n') result.pop_back();
        const char separator = directory ? '\0' : '|';
        std::size_t start = 0;
        while (start < result.size()) {
            const auto end = result.find(separator, start);
            paths.emplace_back(result.substr(start, end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    } else if (failed) {
        *failed = true;
    }
#endif
    return paths;
}

bool open_url(const std::string& url) {
#ifdef _WIN32
    return reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
    @autoreleasepool {
        NSString* text = [NSString stringWithUTF8String:url.c_str()];
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:text]];
    }
#else
    // xdg-open usually returns after handing off to the browser. Reap it outside the UI thread.
    pid_t pid;
    char command[] = "xdg-open";
    char* args[] = {command, const_cast<char*>(url.c_str()), nullptr};
    if (posix_spawnp(&pid, command, nullptr, nullptr, args, environ) != 0) return false;
    std::thread([pid] {
        int status;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
    }).detach();
    return true;
#endif
}

bool open_folder(const std::filesystem::path& directory) {
#ifdef _WIN32
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", directory.c_str(),
                                                    nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#elif defined(__APPLE__)
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:directory.c_str()];
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
    }
#else
    return open_url(directory.string());
#endif
}

bool reveal_file(const std::filesystem::path& file) {
#ifdef _WIN32
    const auto native = ILCreateFromPathW(file.c_str());
    if (!native) return false;
    const auto result = SHOpenFolderAndSelectItems(native, 0, nullptr, 0);
    ILFree(native);
    return SUCCEEDED(result);
#elif defined(__APPLE__)
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:file.c_str()];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:path]]];
        return true;
    }
#else
    return open_folder(file.parent_path());
#endif
}

std::filesystem::path configuration_directory() {
#ifdef _WIN32
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required) {
        std::wstring root(required, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), required);
        if (length && length < required) {
            root.resize(length);
            return std::filesystem::path(root) / "TempShare";
        }
    }
#else
    if (const auto* root = std::getenv("XDG_DATA_HOME")) {
        if (*root) return std::filesystem::u8path(root) / "tempshare";
    }
    if (const auto* home = std::getenv("HOME")) return std::filesystem::u8path(home) / ".local/share/tempshare";
#endif
    return {};
}

std::filesystem::path downloads_directory() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        std::filesystem::path result(raw);
        CoTaskMemFree(raw);
        return result;
    }
#elif defined(__APPLE__)
    @autoreleasepool {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
        if (paths.count) return std::filesystem::u8path([[paths firstObject] UTF8String]);
    }
#else
    if (const auto* root = std::getenv("XDG_DOWNLOAD_DIR")) {
        if (*root) return std::filesystem::u8path(root);
    }
    if (const auto* home = std::getenv("HOME")) return std::filesystem::u8path(home) / "Downloads";
#endif
    return std::filesystem::current_path() / "Downloads";
}

#ifdef _WIN32
namespace {
constexpr UINT tray_message = WM_APP + 114;

NOTIFYICONDATAW tray_data(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    return data;
}

bool add_tray(HWND window) {
    auto data = tray_data(window);
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = tray_message;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!data.hIcon) data.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpynW(data.szTip, L"TempShare", ARRAYSIZE(data.szTip));
    return Shell_NotifyIconW(NIM_ADD, &data);
}

void reveal(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK tray_callback(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto& desktop = *reinterpret_cast<DesktopIntegration*>(ref);
    if (desktop.taskbar_created && msg == desktop.taskbar_created) {
        desktop.tray = add_tray(window);
        if (!desktop.tray) reveal(window);
    }
    if (msg == tray_message) {
        if (lp == NIN_BALLOONUSERCLICK) {
            desktop.clicked_connection = desktop.notice_connection;
            desktop.clicked_sync = desktop.notice_opens_sync;
            desktop.clicked_invitation = desktop.notice_invitation;
        }
        if (lp == NIN_BALLOONUSERCLICK || lp == NIN_BALLOONTIMEOUT || lp == NIN_BALLOONHIDE) {
            desktop.notice_active = false;
            if (desktop.wake) desktop.wake();
        }
        if (lp == WM_LBUTTONUP || lp == NIN_BALLOONUSERCLICK) reveal(window);
        if (lp == WM_RBUTTONUP) {
            POINT cursor{};
            GetCursorPos(&cursor);
            if (const auto menu = CreatePopupMenu()) {
                AppendMenuW(menu, MF_STRING, 1, L"打开 TempShare");
                AppendMenuW(menu, MF_STRING, 2, L"打开接收目录");
                AppendMenuW(menu, MF_STRING, 3, L"检查更新");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 4, L"退出");
                SetForegroundWindow(window);
                const auto action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                                   cursor.x, cursor.y, 0, window, nullptr);
                DestroyMenu(menu);
                PostMessageW(window, WM_NULL, 0, 0);
                switch (action) {
                case 1: reveal(window); break;
                case 2: if (!desktop.inbox.empty()) open_folder(desktop.inbox); break;
                case 3: desktop.updates(); break;
                case 4: reveal(window); PostMessageW(window, WM_CLOSE, 0, 0); break;
                default: break;
                }
            }
        }
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(window, tray_callback, id);
    return DefSubclassProc(window, msg, wp, lp);
}

LRESULT CALLBACK drop_callback(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id,
                               DWORD_PTR data) {
    if (message == WM_DROPFILES) {
        auto& paths = *reinterpret_cast<std::vector<std::filesystem::path>*>(data);
        const auto drop = reinterpret_cast<HDROP>(wparam);
        const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const auto length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, i, path.data(), length + 1)) {
                path.resize(length);
                paths.emplace_back(path);
            }
        }
        DragFinish(drop);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, drop_callback, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

BOOL CALLBACK find_window(HWND window, LPARAM data) {
    wchar_t title[32]{};
    GetWindowTextW(window, title, 32);
    if (std::wstring(title) != L"TempShare") return TRUE;
    *reinterpret_cast<HWND*>(data) = window;
    return FALSE;
}
}

DesktopIntegration::DesktopIntegration(std::vector<std::filesystem::path>& paths, std::function<void()> wake_callback)
    : wake(std::move(wake_callback)) {
    HWND native = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), find_window, reinterpret_cast<LPARAM>(&native));
    if (!native) return;
    window = native;
    if (const auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1))) {
        SendMessageW(native, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(native, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
    SetCurrentProcessExplicitAppUserModelID(L"sudoevolve.TempShare");
    if (SetWindowSubclass(native, drop_callback, 1, reinterpret_cast<DWORD_PTR>(&paths))) DragAcceptFiles(native, TRUE);
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (SetWindowSubclass(native, tray_callback, 2, reinterpret_cast<DWORD_PTR>(this))) tray = add_tray(native);
}

DesktopIntegration::~DesktopIntegration() {
    const auto native = static_cast<HWND>(window);
    if (!native) return;
    if (tray) {
        auto data = tray_data(native);
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    RemoveWindowSubclass(native, tray_callback, 2);
    RemoveWindowSubclass(native, drop_callback, 1);
}

void DesktopIntegration::minimize() const {
    if (window) ShowWindow(static_cast<HWND>(window), tray ? SW_HIDE : SW_MINIMIZE);
}

bool DesktopIntegration::notify(const std::wstring& title, const std::wstring& message, int connection,
                                const std::string& invitation, bool open_sync) {
    if (!tray) return false;
    if (notices.size() >= 64) return false;
    notices.push_back({title, message, connection, invitation, open_sync});
    return true;
}

void DesktopIntegration::pump_notifications() {
    if (!tray || notice_active || notices.empty()) return;
    const auto& notice = notices.front();
    auto data = tray_data(static_cast<HWND>(window));
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    lstrcpynW(data.szInfoTitle, notice.title.c_str(), ARRAYSIZE(data.szInfoTitle));
    lstrcpynW(data.szInfo, notice.message.c_str(), ARRAYSIZE(data.szInfo));
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) return;
    notice_connection = notice.connection;
    notice_opens_sync = notice.sync;
    notice_invitation = notice.invitation;
    notice_active = true;
    notices.pop_front();
}

bool DesktopIntegration::updates() const {
    return open_url("https://github.com/sudoevolve/TempShare/releases/latest");
}
#endif
