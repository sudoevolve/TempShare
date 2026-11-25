#include "platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#elif defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#else
#include <cstdio>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

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

#ifdef _WIN32
namespace {
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

BOOL CALLBACK attach_drop(HWND window, LPARAM data) {
    wchar_t title[32]{};
    GetWindowTextW(window, title, 32);
    if (std::wstring(title) != L"TempShare") return TRUE;
    if (SetWindowSubclass(window, drop_callback, 1, static_cast<DWORD_PTR>(data))) {
        DragAcceptFiles(window, TRUE);
    }
    return FALSE;
}
}

void enable_file_drop(std::vector<std::filesystem::path>& paths) {
    EnumThreadWindows(GetCurrentThreadId(), attach_drop, reinterpret_cast<LPARAM>(&paths));
}
#endif
