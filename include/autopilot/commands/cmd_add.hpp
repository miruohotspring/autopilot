#pragma once

#include <optional>
#include <string>

int cmd_add(
    const std::string& path_arg,
    const std::optional<std::string>& maybe_path_name,
    const std::optional<std::string>& maybe_project_name);
