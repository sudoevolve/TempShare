#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::vector<std::filesystem::path> choose_paths(bool directory);
bool open_url(const std::string& url);
std::filesystem::path configuration_directory();

#ifdef _WIN32
void enable_file_drop(std::vector<std::filesystem::path>& paths);
#endif
