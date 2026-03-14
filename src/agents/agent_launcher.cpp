#include "autopilot/agents/agent_launcher.hpp"
#include "autopilot/projects/project_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string shell_quote(const std::string& s) {
  std::string quoted = "'";
  for (char c : s) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

bool is_executable_file(const fs::path& path) {
  return !path.empty() && fs::exists(path) && !fs::is_directory(path) && ::access(path.c_str(), X_OK) == 0;
}

std::optional<fs::path> find_executable_in_path(const std::string& name) {
  const char* raw_path = std::getenv("PATH");
  if (raw_path == nullptr) {
    return std::nullopt;
  }

  std::stringstream ss(raw_path);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (dir.empty()) {
      continue;
    }
    const fs::path candidate = fs::path(dir) / name;
    if (is_executable_file(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::string build_agent_command(const std::string& agent_name, const std::string& prompt) {
  return shell_quote(agent_name) + " " + shell_quote(prompt);
}

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::optional<std::string> parse_quoted_toml_string(const std::string& raw_value) {
  const std::string value = trim_ascii_whitespace(raw_value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return value.substr(1, value.size() - 2);
  }
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    return value.substr(1, value.size() - 2);
  }
  return std::nullopt;
}

std::optional<std::string> load_configured_agent() {
  const fs::path config_file = autopilot_dir_path() / "config.toml";
  if (!fs::exists(config_file)) {
    return std::nullopt;
  }

  std::ifstream in(config_file);
  if (!in) {
    throw std::runtime_error("failed to open config file: " + config_file.string());
  }

  const std::regex section_re(R"(^\s*\[([A-Za-z0-9_.-]+)\]\s*$)");
  const std::regex key_value_re(R"(^\s*([A-Za-z0-9_.-]+)\s*=\s*(.+?)\s*$)");

  std::string line;
  std::string current_section;
  while (std::getline(in, line)) {
    const std::string trimmed = trim_ascii_whitespace(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    std::smatch section_match;
    if (std::regex_match(trimmed, section_match, section_re)) {
      current_section = section_match[1].str();
      continue;
    }

    if (current_section != "start") {
      continue;
    }

    std::smatch kv_match;
    if (!std::regex_match(trimmed, kv_match, key_value_re)) {
      continue;
    }
    if (kv_match[1].str() != "agent") {
      continue;
    }

    const std::optional<std::string> parsed = parse_quoted_toml_string(kv_match[2].str());
    if (!parsed.has_value()) {
      throw std::runtime_error("invalid start.agent in config.toml");
    }
    return *parsed;
  }

  return std::nullopt;
}

int decode_system_exit_code(const int system_status) {
  if (system_status == -1) {
    return 1;
  }
  if (WIFEXITED(system_status)) {
    return WEXITSTATUS(system_status);
  }
  if (WIFSIGNALED(system_status)) {
    return 128 + WTERMSIG(system_status);
  }
  return 1;
}

} // namespace

std::string resolve_agent_name() {
  const std::optional<std::string> configured_agent = load_configured_agent();
  if (configured_agent.has_value()) {
    if (*configured_agent != "claude" && *configured_agent != "codex") {
      throw std::runtime_error("unsupported agent in config.toml: " + *configured_agent);
    }
    if (!find_executable_in_path(*configured_agent).has_value()) {
      throw std::runtime_error("configured agent CLI not found: " + *configured_agent);
    }
    return *configured_agent;
  }

  for (const char* candidate : {"claude", "codex"}) {
    if (find_executable_in_path(candidate).has_value()) {
      return std::string(candidate);
    }
  }
  throw std::runtime_error("no supported agent CLI found");
}

AgentLaunchResult run_agent(
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const fs::path& stdout_log,
    const fs::path& stderr_log) {
  if (!find_executable_in_path(agent_name).has_value()) {
    throw std::runtime_error("agent CLI not found: " + agent_name);
  }

  const std::string command =
      "cd " + shell_quote(working_directory.string()) + " && " +
      build_agent_command(agent_name, prompt) + " >" + shell_quote(stdout_log.string()) + " 2>" +
      shell_quote(stderr_log.string());

  const int exit_code = decode_system_exit_code(std::system(command.c_str()));
  return AgentLaunchResult{agent_name, exit_code};
}
