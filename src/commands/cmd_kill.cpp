#include "autopilot/commands/cmd_kill.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int cmd_kill() {
  const std::string session = "autopilot";
  const int has_session =
      std::system(("tmux has-session -t " + session + " >/dev/null 2>&1").c_str());
  if (has_session != 0) {
    std::cout << "no tmux session: " << session << '\n';
    return 0;
  }

  const std::string kill_cmd = "tmux kill-session -t " + session;
  if (std::system(kill_cmd.c_str()) != 0) {
    std::cerr << "ap kill failed: failed to kill tmux session\n";
    return 1;
  }

  std::cout << "killed tmux session: " << session << '\n';
  return 0;
}
