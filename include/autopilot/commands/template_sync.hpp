#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>

std::filesystem::path managed_templates_root_from_cwd();
std::optional<std::filesystem::path>
first_missing_managed_template(const std::filesystem::path& template_root);
void sync_managed_templates(
    const std::filesystem::path& template_root,
    const std::filesystem::path& autopilot_dir,
    std::ostream& out);
