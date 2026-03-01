#include "autopilot/commands/cmd_briefing.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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

const char* kColonelSystemPrompt =
    "あなたは autopilot オーケストレーションの colonel です。"
    "general はユーザー、captain は各プロジェクト責任者です。"
    "colonel は実装作業を行わず、統括・報告・相談・判断依頼のみを行います。"
    "general との会話は常に日本語で行ってください。";

const char* kBriefingBootstrapPrompt =
    "Run $self-recognition now. "
    "After self-recognition, be ready to run $briefing and wait for general's instruction. "
    "When asked for briefing, gather ~/.autopilot/projects/*/dashboard.md and report cross-project "
    "status plus blockers that require general's approval or instruction.";

} // namespace

int cmd_briefing() {
  const std::string session = "autopilot";
  const std::string target = session + ":colonel";

  const int has_session =
      std::system(("tmux has-session -t " + session + " >/dev/null 2>&1").c_str());
  if (has_session != 0) {
    const std::string create_session_cmd = "tmux new-session -d -s " + session;
    if (std::system(create_session_cmd.c_str()) != 0) {
      std::cerr << "ap briefing failed: failed to create tmux session\n";
      return 1;
    }
  }

  const int has_colonel_window =
      std::system(("tmux list-windows -t " + session +
                   " -F '#{window_name}' | grep -Fx colonel >/dev/null 2>&1")
                      .c_str());
  if (has_colonel_window != 0) {
    const fs::path autopilot_dir = fs::path(get_home_dir()) / ".autopilot";
    const std::string claude_cmd =
        "claude --model sonnet --dangerously-skip-permissions"
        " --append-system-prompt " +
        shell_quote(kColonelSystemPrompt) + " " + shell_quote(kBriefingBootstrapPrompt);
    const std::string window_cmd = "cd " + shell_quote(autopilot_dir.string()) + " && " + claude_cmd;
    const std::string create_window_cmd =
        "tmux new-window -t " + session + " -n colonel " + shell_quote(window_cmd);
    if (std::system(create_window_cmd.c_str()) != 0) {
      std::cerr << "ap briefing failed: failed to create tmux window\n";
      return 1;
    }
  } else {
    // When reusing the colonel window, push the bootstrap task into the live Claude session.
    const std::string send_keys_cmd =
        "tmux send-keys -t " + target + " " + shell_quote(kBriefingBootstrapPrompt) + " C-m";
    if (std::system(send_keys_cmd.c_str()) != 0) {
      std::cerr << "ap briefing failed: failed to send bootstrap prompt to colonel\n";
      return 1;
    }
  }

  const bool in_tmux = std::getenv("TMUX") != nullptr;
  const std::string attach_cmd =
      in_tmux ? ("tmux switch-client -t " + target) : ("tmux attach-session -t " + target);
  if (std::system(attach_cmd.c_str()) != 0) {
    std::cerr << "ap briefing failed: failed to attach tmux session\n";
    return 1;
  }

  return 0;
}
