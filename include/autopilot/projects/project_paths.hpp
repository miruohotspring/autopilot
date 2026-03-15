#pragma once

#include <filesystem>
#include <string>

std::filesystem::path autopilot_dir_path();
std::filesystem::path projects_dir_path();
std::filesystem::path project_dir_path(const std::string& project_name);
std::filesystem::path project_config_file_path(const std::string& project_name);
std::filesystem::path projects_file_path();
bool autopilot_dir_exists();
