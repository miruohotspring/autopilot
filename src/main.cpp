#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/commands/cmd_new.hpp"

#include <iostream>
#include <optional>
#include <string>

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " init\n";
  std::cerr << "       " << prog << " new [project_name]\n";
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];
  if (cmd == "init") {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }
    return cmd_init();
  }

  if (cmd == "new") {
    if (argc == 2) {
      return cmd_new(std::nullopt);
    }
    return cmd_new(std::string(argv[2]));
  }

  print_usage(argv[0]);
  return 1;
}
