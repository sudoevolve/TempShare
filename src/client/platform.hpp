#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

std::vector<std::filesystem::path> choose_paths(bool directory, bool* failed = nullptr);
bool open_url(const std::string& url);
bool open_folder(const std::filesystem::path& directory);
bool reveal_file(const std::filesystem::path& file);
std::filesystem::path configuration_directory();
std::optional<bool> system_dark_mode();

#ifdef _WIN32
class DesktopIntegration {
public:
    explicit DesktopIntegration(std::vector<std::filesystem::path>& paths);
    ~DesktopIntegration();
    DesktopIntegration(const DesktopIntegration&) = delete;
    DesktopIntegration& operator=(const DesktopIntegration&) = delete;
    void notify(const std::wstring& title, const std::wstring& message) const;
    void minimize() const;
    bool updates(bool automatic = false) const;
    std::filesystem::path inbox;
    void* window = nullptr;
    unsigned taskbar_created = 0;
    bool tray = false;
};
#endif
