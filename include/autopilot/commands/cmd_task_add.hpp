#pragma once

#include <optional>
#include <string>

int cmd_task_add(
    const std::string& title, const std::optional<std::string>& maybe_project_name);
