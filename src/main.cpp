#include "autopilot/commands/cmd_add.hpp"
#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/commands/cmd_delete.hpp"
#include "autopilot/commands/cmd_list.hpp"
#include "autopilot/commands/cmd_new.hpp"
#include "autopilot/commands/cmd_rm.hpp"

#include <iostream>
#include <optional>
#include <string>

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " init\n";
  std::cerr << "       " << prog << " new [project_name]\n";
  std::cerr << "       " << prog << " delete [project_name]\n";
  std::cerr << "       " << prog << " list\n";
  std::cerr << "       " << prog << " add <path> [-p <project_name>]\n";
  std::cerr << "       " << prog << " rm [-p <project_name>]\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
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
    if (argc == 3) {
      return cmd_new(std::string(argv[2]));
    }
    print_usage(argv[0]);
    return 1;
  }

  if (cmd == "delete") {
    if (argc == 2) {
      return cmd_delete(std::nullopt);
    }
    if (argc == 3) {
      return cmd_delete(std::string(argv[2]));
    }
    print_usage(argv[0]);
    return 1;
  }

  if (cmd == "list") {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }
    return cmd_list();
  }

  if (cmd == "add") {
    if (argc == 3) {
      return cmd_add(std::string(argv[2]), std::nullopt);
    }
    if (argc == 5 && std::string(argv[3]) == "-p") {
      return cmd_add(std::string(argv[2]), std::string(argv[4]));
    }
    print_usage(argv[0]);
    return 1;
  }

  if (cmd == "rm") {
    if (argc == 2) {
      return cmd_rm(std::nullopt);
    }
    if (argc == 4 && std::string(argv[2]) == "-p") {
      return cmd_rm(std::string(argv[3]));
    }
    print_usage(argv[0]);
    return 1;
  }

  print_usage(argv[0]);
  return 1;
}
