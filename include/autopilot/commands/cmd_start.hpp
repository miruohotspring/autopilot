#pragma once

#include <optional>
#include <string>

int cmd_start(
    const std::optional<std::string>& maybe_project_name,
    std::optional<int> maybe_timeout_seconds = std::nullopt);
