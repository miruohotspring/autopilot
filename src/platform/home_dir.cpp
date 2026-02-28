#include "autopilot/platform/home_dir.hpp"

#include <cstdlib>
#include <pwd.h>
#include <stdexcept>
#include <sys/types.h>
#include <unistd.h>

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
