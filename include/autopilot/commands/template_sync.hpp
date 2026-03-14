#pragma once

#include <array>
#include <filesystem>
#include <iosfwd>
#include <optional>

using ManagedTemplateList = std::array<const char*, 5>;

std::filesystem::path managed_templates_root_from_cwd();
const ManagedTemplateList& managed_template_relative_paths();
std::optional<std::filesystem::path>
first_missing_managed_template(const std::filesystem::path& template_root);
void sync_managed_templates(
    const std::filesystem::path& template_root,
    const std::filesystem::path& autopilot_dir,
    std::ostream& out);
