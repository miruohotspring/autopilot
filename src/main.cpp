#include "autopilot/commands/cmd_add.hpp"
#include "autopilot/commands/cmd_start.hpp"
#include "autopilot/commands/cmd_workflow.hpp"
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
  std::cerr << "       " << prog << " task sync [project_name]\n";
  std::cerr << "       " << prog << " task next [project_name]\n";
  std::cerr << "       " << prog << " task set-status <task_id> --status <status> [-p <project_name>]\n";
  std::cerr << "       " << prog << " rm [-p <project_name>]\n";
  std::cerr << "       " << prog << " recover [project_name]\n";
  std::cerr << "       " << prog << " start [project_name] [--review] [--no-review] [--reviewer-agent <agent>] [--timeout <seconds>]\n";
  std::cerr << "       " << prog << " run start [project_name] [--task <task_id>] [--actor <actor>]\n";
  std::cerr << "       " << prog << " run finish <run_id> --status <done|failed|blocked|timeout|cancelled> [--review] [--no-review] [--summary <text>] [--blocker-reason <text>] [--blocker-category <category>] [--approval-required]\n";
  std::cerr << "       " << prog << " review start <coder_run_id> [--actor <actor>]\n";
  std::cerr << "       " << prog << " review submit <reviewer_run_id> --verdict <approve|rework|blocked> [--summary <text>] [--issue <text>]... [--suggestion <text>]... [--reason <text>] [--category <category>]\n";
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
    if (argc < 3) {
      print_usage(argv[0]);
      return 1;
    }

    const std::string subcmd = argv[2];
    if (subcmd == "add") {
      if (argc < 4) {
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

    if (subcmd == "sync") {
      if (argc == 3) {
        return cmd_task_sync(std::nullopt);
      }
      if (argc == 4) {
        return cmd_task_sync(std::string(argv[3]));
      }
      print_usage(argv[0]);
      return 1;
    }

    if (subcmd == "next") {
      if (argc == 3) {
        return cmd_task_next(std::nullopt);
      }
      if (argc == 4) {
        return cmd_task_next(std::string(argv[3]));
      }
      print_usage(argv[0]);
      return 1;
    }

    if (subcmd == "set-status") {
      if (argc < 6) {
        print_usage(argv[0]);
        return 1;
      }

      const std::string task_id = argv[3];
      std::optional<std::string> maybe_project_name;
      std::optional<std::string> maybe_status;
      for (int i = 4; i < argc; i += 2) {
        if (i + 1 >= argc) {
          print_usage(argv[0]);
          return 1;
        }
        const std::string flag = argv[i];
        const std::string value = argv[i + 1];
        if (flag == "-p") {
          maybe_project_name = value;
        } else if (flag == "--status") {
          maybe_status = value;
        } else {
          print_usage(argv[0]);
          return 1;
        }
      }
      if (!maybe_status.has_value()) {
        print_usage(argv[0]);
        return 1;
      }
      return cmd_task_set_status(maybe_project_name, task_id, *maybe_status);
    }

    print_usage(argv[0]);
    return 1;
  }

  if (cmd == "start") {
    std::optional<std::string> maybe_project;
    std::optional<int> maybe_timeout;
    std::optional<std::string> maybe_reviewer_agent;
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
      } else if (arg == "--reviewer-agent" && i + 1 < argc) {
        maybe_reviewer_agent = argv[i + 1];
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
    return cmd_start(maybe_project, maybe_timeout, maybe_review_enabled, maybe_reviewer_agent);
  }

  if (cmd == "recover") {
    if (argc == 2) {
      return cmd_recover(std::nullopt);
    }
    if (argc == 3) {
      return cmd_recover(std::string(argv[2]));
    }
    print_usage(argv[0]);
    return 1;
  }

  if (cmd == "run") {
    if (argc < 3) {
      print_usage(argv[0]);
      return 1;
    }
    const std::string subcmd = argv[2];
    if (subcmd == "start") {
      std::optional<std::string> maybe_project_name;
      std::optional<std::string> maybe_task_id;
      std::optional<std::string> maybe_actor_name;
      int i = 3;
      while (i < argc) {
        const std::string arg = argv[i];
        if (arg == "--task" && i + 1 < argc) {
          maybe_task_id = argv[i + 1];
          i += 2;
        } else if (arg == "--actor" && i + 1 < argc) {
          maybe_actor_name = argv[i + 1];
          i += 2;
        } else if (arg.rfind("--", 0) != 0 && !maybe_project_name.has_value()) {
          maybe_project_name = arg;
          ++i;
        } else {
          print_usage(argv[0]);
          return 1;
        }
      }
      return cmd_run_start(maybe_project_name, maybe_task_id, maybe_actor_name);
    }

    if (subcmd == "finish") {
      if (argc < 4) {
        print_usage(argv[0]);
        return 1;
      }
      const std::string run_id = argv[3];
      std::optional<std::string> maybe_status;
      std::optional<std::string> maybe_summary;
      std::optional<std::string> maybe_blocker_reason;
      std::optional<std::string> maybe_blocker_category;
      std::optional<bool> maybe_review_enabled;
      bool approval_required = false;
      int i = 4;
      while (i < argc) {
        const std::string arg = argv[i];
        if (arg == "--status" && i + 1 < argc) {
          maybe_status = argv[i + 1];
          i += 2;
        } else if (arg == "--summary" && i + 1 < argc) {
          maybe_summary = argv[i + 1];
          i += 2;
        } else if (arg == "--blocker-reason" && i + 1 < argc) {
          maybe_blocker_reason = argv[i + 1];
          i += 2;
        } else if (arg == "--blocker-category" && i + 1 < argc) {
          maybe_blocker_category = argv[i + 1];
          i += 2;
        } else if (arg == "--review") {
          maybe_review_enabled = true;
          ++i;
        } else if (arg == "--no-review") {
          maybe_review_enabled = false;
          ++i;
        } else if (arg == "--approval-required") {
          approval_required = true;
          ++i;
        } else {
          print_usage(argv[0]);
          return 1;
        }
      }
      if (!maybe_status.has_value()) {
        print_usage(argv[0]);
        return 1;
      }
      return cmd_run_finish(
          run_id,
          *maybe_status,
          maybe_review_enabled,
          maybe_summary,
          maybe_blocker_reason,
          maybe_blocker_category,
          approval_required);
    }

    print_usage(argv[0]);
    return 1;
  }

  if (cmd == "review") {
    if (argc < 4) {
      print_usage(argv[0]);
      return 1;
    }
    const std::string subcmd = argv[2];
    if (subcmd == "start") {
      const std::string coder_run_id = argv[3];
      std::optional<std::string> maybe_actor_name;
      if (argc == 6 && std::string(argv[4]) == "--actor") {
        maybe_actor_name = argv[5];
      } else if (argc != 4) {
        print_usage(argv[0]);
        return 1;
      }
      return cmd_review_start(coder_run_id, maybe_actor_name);
    }

    if (subcmd == "submit") {
      const std::string reviewer_run_id = argv[3];
      std::optional<std::string> maybe_verdict;
      std::optional<std::string> maybe_summary;
      std::optional<std::string> maybe_reason;
      std::optional<std::string> maybe_category;
      std::vector<std::string> issues;
      std::vector<std::string> suggestions;
      int i = 4;
      while (i < argc) {
        const std::string arg = argv[i];
        if (arg == "--verdict" && i + 1 < argc) {
          maybe_verdict = argv[i + 1];
          i += 2;
        } else if (arg == "--summary" && i + 1 < argc) {
          maybe_summary = argv[i + 1];
          i += 2;
        } else if (arg == "--issue" && i + 1 < argc) {
          issues.push_back(argv[i + 1]);
          i += 2;
        } else if (arg == "--suggestion" && i + 1 < argc) {
          suggestions.push_back(argv[i + 1]);
          i += 2;
        } else if (arg == "--reason" && i + 1 < argc) {
          maybe_reason = argv[i + 1];
          i += 2;
        } else if (arg == "--category" && i + 1 < argc) {
          maybe_category = argv[i + 1];
          i += 2;
        } else {
          print_usage(argv[0]);
          return 1;
        }
      }
      if (!maybe_verdict.has_value()) {
        print_usage(argv[0]);
        return 1;
      }
      return cmd_review_submit(
          reviewer_run_id,
          *maybe_verdict,
          maybe_summary,
          issues,
          suggestions,
          maybe_reason,
          maybe_category);
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
