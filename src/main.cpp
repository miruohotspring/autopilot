#include "autopilot/commands/cmd_add.hpp"
#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/commands/cmd_delete.hpp"
#include "autopilot/commands/cmd_list.hpp"
#include "autopilot/commands/cmd_new.hpp"
#include "autopilot/commands/cmd_rm.hpp"
#include "autopilot/commands/cmd_briefing.hpp"
#include "autopilot/commands/cmd_update.hpp"

#include <iostream>
#include <optional>
#include <string>

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " init\n";
  std::cerr << "       " << prog << " new [project_name]\n";
  std::cerr << "       " << prog << " delete [project_name]\n";
  std::cerr << "       " << prog << " list\n";
  std::cerr << "       " << prog << " add <path> [-n <name>] [-p <project_name>]\n";
  std::cerr << "       " << prog << " rm [-p <project_name>]\n";
  std::cerr << "       " << prog << " briefing\n";
  std::cerr << "       " << prog << " update\n";
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
    if (argc < 3) {
      print_usage(argv[0]);
      return 1;
    }

    const std::string path_arg = argv[2];
    std::optional<std::string> maybe_project_name;
    std::optional<std::string> maybe_path_name;
    for (int i = 3; i < argc; i += 2) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 1;
      }
      const std::string flag = argv[i];
      const std::string value = argv[i + 1];
      if (flag == "-p") {
        maybe_project_name = value;
      } else if (flag == "-n") {
        maybe_path_name = value;
      } else {
        print_usage(argv[0]);
        return 1;
      }
    }

    return cmd_add(path_arg, maybe_path_name, maybe_project_name);
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

  if (cmd == "briefing") {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }
    return cmd_briefing();
  }

  if (cmd == "update") {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }
    return cmd_update();
  }

  print_usage(argv[0]);
  return 1;
}
