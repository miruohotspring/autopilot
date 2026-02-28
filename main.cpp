#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

std::string get_home_dir() {
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::string(home);
  }

  const passwd* pw = getpwuid(getuid());
  if (pw != nullptr && pw->pw_dir != nullptr && pw->pw_dir[0] != '\0') {
    return std::string(pw->pw_dir);
  }

  throw std::runtime_error("failed to determine home directory");
}

int cmd_init() {
  const fs::path home = get_home_dir();
  const fs::path autopilot_dir = home / ".autopilot";
  const fs::path backup_dir = home / ".autopilot.bak";

  try {
    if (fs::exists(autopilot_dir)) {
      if (fs::exists(backup_dir)) {
        std::cerr << "ap init failed: backup already exists: " << backup_dir << '\n';
        return 1;
      }
      fs::rename(autopilot_dir, backup_dir);
      std::cout << "renamed: " << autopilot_dir << " -> " << backup_dir << '\n';
    }

    fs::create_directories(autopilot_dir);
    std::cout << "created: " << autopilot_dir << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap init failed: " << e.what() << '\n';
    return 1;
  }
}

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " init\n";
}

int main(int argc, char** argv) {
  if (argc != 2) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];
  if (cmd == "init") {
    return cmd_init();
  }

  print_usage(argv[0]);
  return 1;
}
