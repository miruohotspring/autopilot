#include "autopilot/commands/cmd_add.hpp"
#include "autopilot/commands/cmd_start.hpp"
#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/commands/cmd_delete.hpp"
#include "autopilot/commands/cmd_list.hpp"
#include "autopilot/commands/cmd_new.hpp"
#include "autopilot/commands/cmd_rm.hpp"
#include "autopilot/commands/cmd_task_add.hpp"
#include "autopilot/commands/cmd_briefing.hpp"
#include "autopilot/commands/cmd_kill.hpp"
#include "autopilot/commands/cmd_update.hpp"

#include <iostream>
#include <optional>
#include <string>

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " init\n";
  std::cerr << "       " << prog << " new [project_name] [slug]\n";
  std::cerr << "       " << prog << " delete [project_name]\n";
  std::cerr << "       " << prog << " list\n";
  std::cerr << "       " << prog << " add <path> [-n <name>] [-p <project_name>]\n";
  std::cerr << "       " << prog << " task add <title> [-p <project_name>]\n";
  std::cerr << "       " << prog << " rm [-p <project_name>]\n";
  std::cerr << "       " << prog << " start [project_name] [--review] [--no-review] [--timeout <seconds>]\n";
  std::cerr << "       " << prog << " briefing\n";
  std::cerr << "       " << prog << " kill\n";
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
      return cmd_new(std::nullopt, std::nullopt);
    }
    if (argc == 3) {
      return cmd_new(std::string(argv[2]), std::nullopt);
    }
    if (argc == 4) {
      return cmd_new(std::string(argv[2]), std::string(argv[3]));
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

  if (cmd == "task") {
    if (argc < 4 || std::string(argv[2]) != "add") {
      print_usage(argv[0]);
      return 1;
    }

    std::optional<std::string> maybe_project_name;
    std::string title = argv[3];
    for (int i = 4; i < argc; i += 2) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return 1;
      }
      const std::string flag = argv[i];
      const std::string value = argv[i + 1];
      if (flag == "-p") {
        maybe_project_name = value;
      } else {
        print_usage(argv[0]);
        return 1;
      }
    }

    return cmd_task_add(title, maybe_project_name);
  }

  if (cmd == "start") {
    std::optional<std::string> maybe_project;
    std::optional<int> maybe_timeout;
    bool requested_review = false;
    bool requested_no_review = false;
    int i = 2;
    while (i < argc) {
      const std::string arg = argv[i];
      if (arg == "--timeout" && i + 1 < argc) {
        try {
          maybe_timeout = std::stoi(argv[i + 1]);
        } catch (const std::exception&) {
          print_usage(argv[0]);
          return 1;
        }
        i += 2;
      } else if (arg == "--review") {
        requested_review = true;
        ++i;
      } else if (arg == "--no-review") {
        requested_no_review = true;
        ++i;
      } else if (arg.rfind("--", 0) != 0 && !maybe_project.has_value()) {
        maybe_project = arg;
        ++i;
      } else {
        print_usage(argv[0]);
        return 1;
      }
    }
    std::optional<bool> maybe_review_enabled;
    if (requested_review) {
      maybe_review_enabled = true;
    } else if (requested_no_review) {
      maybe_review_enabled = false;
    }
    return cmd_start(maybe_project, maybe_timeout, maybe_review_enabled);
  }

  if (cmd == "briefing") {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }
    return cmd_briefing();
  }

  if (cmd == "kill") {
    if (argc != 2) {
      print_usage(argv[0]);
      return 1;
    }
    return cmd_kill();
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
