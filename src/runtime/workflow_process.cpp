#include "autopilot/runtime/workflow.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

std::string sanitize_tmux_name(std::string s) {
  for (char& ch : s) {
    const bool is_alnum =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (!is_alnum && ch != '-' && ch != '_') {
      ch = '-';
    }
  }
  return s;
}

bool tmux_is_available() {
  return std::getenv("AUTOPILOT_START_DISABLE_TMUX") == nullptr &&
         std::system("tmux -V >/dev/null 2>&1") == 0;
}

void touch_empty_file(const fs::path& path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to create file: " + path.string());
  }
}

int read_exit_code_file(const fs::path& exit_code_file) {
  std::ifstream in(exit_code_file);
  if (!in) {
    throw std::runtime_error("failed to read exit code file: " + exit_code_file.string());
  }
  int exit_code = 1;
  in >> exit_code;
  return exit_code;
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

pid_t launch_bash_command_async(const std::string& command) {
  const pid_t child_pid = ::fork();
  if (child_pid < 0) {
    throw std::runtime_error("failed to fork runner process");
  }
  if (child_pid == 0) {
    ::execl("/bin/bash", "bash", "-lc", command.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return child_pid;
}

int wait_for_process(const pid_t pid) {
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    throw std::runtime_error("failed to wait for runner process");
  }
  return decode_system_exit_code(status);
}

std::optional<int> wait_for_pid_file(const fs::path& pid_file, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (fs::exists(pid_file)) {
      std::ifstream in(pid_file);
      int pid = 0;
      if (in >> pid && pid > 0) {
        return pid;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

std::string build_logged_agent_command(
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const std::string& stdout_redirection,
    const std::string& stderr_redirection) {
  return "cd " + shell_quote(working_directory.string()) + " && exec " +
         build_agent_shell_command(agent_name, prompt) + stdout_redirection + stderr_redirection;
}

std::string build_timed_shell_fragment(
    const std::string& command,
    const fs::path& timeout_marker_file,
    int timeout_seconds) {
  if (timeout_seconds <= 0) {
    return command + "; status=$?";
  }

  const std::string marker = shell_quote(timeout_marker_file.string());
  const std::string wrapped_command = shell_quote("set +e; " + command);
  std::ostringstream oss;
  oss << "rm -f " << marker << "; ";
  oss << "setsid bash -lc " << wrapped_command << " & cmd_pid=$!; ";
  oss << "(sleep " << timeout_seconds
      << "; if kill -0 \"$cmd_pid\" 2>/dev/null; then : > " << marker
      << "; kill -TERM -- -\"$cmd_pid\" 2>/dev/null; sleep 1; "
      << "kill -KILL -- -\"$cmd_pid\" 2>/dev/null || true; "
      << "fi) & watchdog_pid=$!; ";
  oss << "wait \"$cmd_pid\"; status=$?; ";
  oss << "kill \"$watchdog_pid\" 2>/dev/null || true; ";
  oss << "wait \"$watchdog_pid\" 2>/dev/null || true";
  return oss.str();
}

AgentLaunchResult run_agent_in_tmux(
    const std::string& project_name,
    const std::string& run_id,
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const fs::path& stdout_log,
    const fs::path& stderr_log,
    int timeout_seconds,
    const std::function<void(int)>& on_runner_started) {
  const std::string session = "autopilot";
  const std::string window_name = sanitize_tmux_name("start-" + project_name + "-" + run_id);
  const std::string target = session + ":" + window_name;
  const std::string channel = sanitize_tmux_name("ap-start-" + project_name + "-" + run_id);
  const fs::path exit_code_file = stdout_log.parent_path() / "agent.exit";
  const fs::path runner_pid_file = stdout_log.parent_path() / "runner.pid";

  touch_empty_file(stdout_log);
  touch_empty_file(stderr_log);
  touch_empty_file(exit_code_file);
  fs::remove(runner_pid_file);

  const bool session_exists =
      std::system(("tmux has-session -t " + session + " >/dev/null 2>&1").c_str()) == 0;

  std::string stdout_display;
  if (agent_name == "claude") {
    const fs::path filter_script = stdout_log.parent_path() / "display_filter.py";
    write_text_file(filter_script, "import sys\nimport json\n\nfor line in sys.stdin:\n"
                                   "    line = line.strip()\n"
                                   "    if not line:\n"
                                   "        continue\n"
                                   "    try:\n"
                                   "        event = json.loads(line)\n"
                                   "    except Exception:\n"
                                   "        continue\n"
                                   "    if event.get(\"type\") != \"assistant\":\n"
                                   "        continue\n"
                                   "    for block in event.get(\"message\", {}).get(\"content\", []):\n"
                                   "        block_type = block.get(\"type\")\n"
                                   "        if block_type == \"thinking\":\n"
                                   "            text = block.get(\"thinking\", \"\").strip()\n"
                                   "            if text:\n"
                                   "                print(\"[thinking] \" + text, flush=True)\n"
                                   "        elif block_type == \"text\":\n"
                                   "            text = block.get(\"text\", \"\").strip()\n"
                                   "            if text:\n"
                                   "                print(text, flush=True)\n"
                                   "        elif block_type == \"tool_use\":\n"
                                   "            name = block.get(\"name\", \"?\")\n"
                                   "            inp = block.get(\"input\", {})\n"
                                   "            value = next(iter(inp.values()), \"\") if inp else \"\"\n"
                                   "            print(\"[\" + name + \"] \" + str(value)[:60], flush=True)\n");
    stdout_display = " | python3 " + shell_quote(filter_script.string());
  }

  const std::string agent_command = build_logged_agent_command(
      agent_name,
      prompt,
      working_directory,
      " > >(tee " + shell_quote(stdout_log.string()) + stdout_display + ")",
      " 2> >(tee " + shell_quote(stderr_log.string()) + " >&2)");
  const std::string timed_agent_command = build_timed_shell_fragment(
      agent_command,
      stdout_log.parent_path() / "agent.timeout",
      timeout_seconds);
  const std::string worker_script =
      "set +e; printf '%s\\n' \"$$\" > " + shell_quote(runner_pid_file.string()) + "; " +
      timed_agent_command + "; printf '%s\\n' \"$status\" > " +
      shell_quote(exit_code_file.string()) + "; tmux wait-for -S " + shell_quote(channel) +
      "; tmux kill-window -t " + shell_quote(target);
  const std::string tmux_command = "bash -lc " + shell_quote(worker_script);

  if (!session_exists) {
    const std::string create_session_cmd =
        "tmux new-session -d -s " + session + " -n " + shell_quote(window_name) + " " +
        shell_quote(tmux_command);
    if (std::system(create_session_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to create tmux session");
    }
  } else {
    const std::string create_window_cmd =
        "tmux new-window -d -t " + session + " -n " + shell_quote(window_name) + " " +
        shell_quote(tmux_command);
    if (std::system(create_window_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to create tmux window");
    }
  }

  const std::optional<int> runner_pid = wait_for_pid_file(runner_pid_file);
  if (!runner_pid.has_value()) {
    throw std::runtime_error("failed to read tmux runner pid");
  }
  on_runner_started(*runner_pid);
  fs::remove(runner_pid_file);

  if (std::getenv("TMUX") != nullptr) {
    const std::string switch_cmd = "tmux switch-client -t " + shell_quote(target);
    if (std::system(switch_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to switch tmux client");
    }
    if (std::system(("tmux wait-for " + shell_quote(channel)).c_str()) != 0) {
      throw std::runtime_error("failed to wait for tmux task completion");
    }
  } else if (!session_exists) {
    const std::string attach_cmd = "tmux attach-session -t " + shell_quote(target);
    if (std::system(attach_cmd.c_str()) != 0) {
      throw std::runtime_error("failed to attach tmux session");
    }
  } else {
    std::cout << "started task window: " << target << '\n';
    if (std::system(("tmux wait-for " + shell_quote(channel)).c_str()) != 0) {
      throw std::runtime_error("failed to wait for tmux task completion");
    }
  }

  const int exit_code = read_exit_code_file(exit_code_file);
  fs::remove(exit_code_file);
  return AgentLaunchResult{agent_name, exit_code};
}

AgentLaunchResult run_agent_with_timeout(
    const std::string& agent_name,
    const std::string& prompt,
    const fs::path& working_directory,
    const fs::path& stdout_log,
    const fs::path& stderr_log,
    const fs::path& timeout_marker_file,
    int timeout_seconds) {
  const std::string base_cmd = build_logged_agent_command(
      agent_name,
      prompt,
      working_directory,
      " >" + shell_quote(stdout_log.string()),
      " 2>" + shell_quote(stderr_log.string()));
  const std::string wrapped_command =
      "set +e; " +
      build_timed_shell_fragment(base_cmd, timeout_marker_file, timeout_seconds) +
      "; exit \"$status\"";
  const pid_t runner_pid = launch_bash_command_async(wrapped_command);
  return AgentLaunchResult{agent_name, wait_for_process(runner_pid)};
}
