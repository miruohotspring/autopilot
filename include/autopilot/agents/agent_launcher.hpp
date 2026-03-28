#pragma once

#include <filesystem>
#include <optional>
#include <string>

struct AgentLaunchResult {
  std::string agent_name;
  int exit_code;
};

std::string resolve_agent_name();
std::string resolve_reviewer_agent_name(
    const std::optional<std::string>& cli_override,
    const std::string& coder_agent_name);
std::string build_agent_shell_command(const std::string& agent_name, const std::string& prompt);

AgentLaunchResult run_agent(
    const std::string& agent_name,
    const std::string& prompt,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& stdout_log,
    const std::filesystem::path& stderr_log);
