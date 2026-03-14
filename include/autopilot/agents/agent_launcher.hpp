#pragma once

#include <filesystem>
#include <string>

struct AgentLaunchResult {
  std::string agent_name;
  int exit_code;
};

std::string resolve_agent_name();

AgentLaunchResult run_agent(
    const std::string& agent_name,
    const std::string& prompt,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& stdout_log,
    const std::filesystem::path& stderr_log);
