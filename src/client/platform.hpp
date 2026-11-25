#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::vector<std::filesystem::path> choose_paths(bool directory, bool* failed = nullptr);
bool open_url(const std::string& url);
bool open_folder(const std::filesystem::path& directory);
std::filesystem::path configuration_directory();

#ifdef _WIN32
void enable_file_drop(std::vector<std::filesystem::path>& paths);
#endif
