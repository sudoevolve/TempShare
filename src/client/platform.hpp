#pragma once

#include <filesystem>
#include <functional>
#include <deque>
#include <optional>
#include <string>
#include <vector>

std::vector<std::filesystem::path> choose_paths(bool directory, bool* failed = nullptr);
bool open_url(const std::string& url);
bool open_folder(const std::filesystem::path& directory);
bool reveal_file(const std::filesystem::path& file);
std::filesystem::path configuration_directory();
std::filesystem::path downloads_directory();
std::optional<bool> system_dark_mode();

#ifdef _WIN32
class DesktopIntegration {
public:
    DesktopIntegration(std::vector<std::filesystem::path>& paths, std::function<void()> wake);
    ~DesktopIntegration();
    DesktopIntegration(const DesktopIntegration&) = delete;
    DesktopIntegration& operator=(const DesktopIntegration&) = delete;
    bool notify(const std::wstring& title, const std::wstring& message, int connection, const std::string& invitation, bool open_sync = false);
    void pump_notifications();
    void minimize() const;
    bool updates() const;
    std::filesystem::path inbox;
    void* window = nullptr;
    unsigned taskbar_created = 0;
    bool tray = false;
    std::function<void()> wake;
    int notice_connection = -1;
    int clicked_connection = -1;
    bool notice_opens_sync = false;
    bool clicked_sync = false;
    std::string notice_invitation, clicked_invitation;
    struct Notice { std::wstring title, message; int connection; std::string invitation; bool sync; };
    std::deque<Notice> notices;
    bool notice_active = false;
};
#endif
