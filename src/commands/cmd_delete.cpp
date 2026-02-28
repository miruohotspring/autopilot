#include "autopilot/commands/cmd_delete.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

bool is_valid_project_name(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  if (name.front() == '-' || name.back() == '-') {
    return false;
  }

  for (const unsigned char c : name) {
    if (!(std::isalnum(c) || c == '-')) {
      return false;
    }
  }
  return true;
}

std::string trim_ascii_whitespace(const std::string& s) {
  const std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string lowercase_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

std::set<std::string> load_top_level_projects(const fs::path& projects_file) {
  std::set<std::string> projects;
  std::ifstream in(projects_file);
  if (!in) {
    throw std::runtime_error("failed to open projects file: " + projects_file.string());
  }

  const std::regex top_level_key_re(R"(^([A-Za-z0-9-]+)\s*:)");
  std::string line;
  std::smatch match;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#') {
      continue;
    }
    if (std::regex_search(line, match, top_level_key_re)) {
      projects.insert(match[1].str());
    }
  }

  return projects;
}

bool is_interactive_terminal() {
  return ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
}

std::optional<std::string> select_project_to_delete_with_prompt(const std::vector<std::string>& project_list) {
  if (project_list.empty()) {
    return std::nullopt;
  }

  std::cout << "Select project to delete:\n";
  for (std::size_t i = 0; i < project_list.size(); ++i) {
    std::cout << "  " << (i + 1) << ") " << project_list[i] << '\n';
  }

  while (true) {
    std::cout << "Enter number to delete: ";
    std::cout.flush();

    std::string answer;
    if (!std::getline(std::cin, answer)) {
      return std::nullopt;
    }

    const std::string normalized = trim_ascii_whitespace(answer);
    try {
      std::size_t pos = 0;
      const unsigned long selected = std::stoul(normalized, &pos, 10);
      if (pos == normalized.size() && selected >= 1 && selected <= project_list.size()) {
        return project_list[selected - 1];
      }
    } catch (const std::exception&) {
    }

    std::cout << "Please enter a number between 1 and " << project_list.size() << ".\n";
  }
}

std::optional<std::string> select_project_to_delete_with_tui(std::vector<std::string> project_list) {
  if (project_list.empty()) {
    return std::nullopt;
  }

  const std::string prompt = "? Select project to delete (Use arrow keys or j/k)";
  std::size_t max_width = prompt.size();
  for (const std::string& project_name : project_list) {
    max_width = std::max(max_width, project_name.size() + 3U);
  }
  const auto terminal_size = ftxui::Terminal::Size();
  const int ui_width = std::max(1, std::min<int>(static_cast<int>(max_width), terminal_size.dimx));
  const int ui_height = std::max(2, std::min<int>(static_cast<int>(project_list.size()) + 1, terminal_size.dimy));

  int selected = 0;
  bool confirmed = false;
  auto screen = ftxui::ScreenInteractive::FixedSize(ui_width, ui_height);

  ftxui::MenuOption menu_option;
  menu_option.entries_option.transform = [](const ftxui::EntryState& state) {
    ftxui::Element entry = ftxui::text(state.label);
    // Keep terminal theme colors intact: use text attributes instead of fixed colors.
    if (state.active) {
      return entry | ftxui::bold;
    }
    return entry | ftxui::dim;
  };
  menu_option.on_enter = [&] {
    confirmed = true;
    screen.ExitLoopClosure()();
  };

  ftxui::Component menu = ftxui::Menu(&project_list, &selected, menu_option);
  ftxui::Component with_vim_keys = ftxui::CatchEvent(menu, [&](ftxui::Event event) {
    if (event == ftxui::Event::Character("j")) {
      return menu->OnEvent(ftxui::Event::ArrowDown);
    }
    if (event == ftxui::Event::Character("k")) {
      return menu->OnEvent(ftxui::Event::ArrowUp);
    }
    return false;
  });

  ftxui::Component ui = ftxui::Renderer(with_vim_keys, [&] {
    return ftxui::vbox({
        ftxui::text(prompt) | ftxui::bold,
        with_vim_keys->Render(),
    });
  });

  screen.Loop(ui);
  if (!confirmed) {
    return std::nullopt;
  }
  return project_list[static_cast<std::size_t>(selected)];
}

std::optional<std::string> select_project_to_delete(const std::set<std::string>& projects) {
  const std::vector<std::string> project_list(projects.begin(), projects.end());
  if (is_interactive_terminal()) {
    return select_project_to_delete_with_tui(project_list);
  }
  return select_project_to_delete_with_prompt(project_list);
}

std::optional<bool> confirm_delete(const std::string& project_name) {
  while (true) {
    std::cout << "Delete project '" << project_name << "'? [y/n]: ";
    std::cout.flush();

    std::string answer;
    if (!std::getline(std::cin, answer)) {
      return std::nullopt;
    }

    const std::string normalized = lowercase_ascii(trim_ascii_whitespace(answer));
    if (normalized == "y" || normalized == "yes") {
      return true;
    }
    if (normalized == "n" || normalized == "no") {
      return false;
    }
    std::cout << "Please answer y or n.\n";
  }
}

bool remove_project_block(const fs::path& projects_file, const std::string& project_name) {
  std::ifstream in(projects_file);
  if (!in) {
    throw std::runtime_error("failed to open projects file: " + projects_file.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  in.close();

  const std::regex top_level_key_re(R"(^([A-Za-z0-9-]+)\s*:)");
  std::vector<std::string> kept;
  kept.reserve(lines.size());

  bool found = false;
  bool dropping_block = false;
  for (const std::string& current_line : lines) {
    if (dropping_block) {
      std::smatch match;
      if (std::regex_search(current_line, match, top_level_key_re)) {
        dropping_block = false;
      } else if (!current_line.empty() && current_line[0] != ' ' && current_line[0] != '\t') {
        dropping_block = false;
      } else {
        continue;
      }
    }

    std::smatch match;
    if (std::regex_search(current_line, match, top_level_key_re) && match[1].str() == project_name) {
      found = true;
      dropping_block = true;
      continue;
    }

    kept.push_back(current_line);
  }

  if (!found) {
    return false;
  }

  std::ofstream out(projects_file, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open projects file for write: " + projects_file.string());
  }

  for (const std::string& kept_line : kept) {
    out << kept_line << '\n';
  }

  return true;
}

}  // namespace

int cmd_delete(const std::optional<std::string>& maybe_project_name) {
  if (maybe_project_name.has_value() && !is_valid_project_name(*maybe_project_name)) {
    std::cerr << "invalid project name\n";
    return 1;
  }

  const fs::path home = get_home_dir();
  const fs::path autopilot_dir = home / ".autopilot";
  if (!fs::exists(autopilot_dir) || !fs::is_directory(autopilot_dir)) {
    std::cerr << "Please run ap init first\n";
    return 1;
  }

  const fs::path projects_file = autopilot_dir / "projects.yaml";
  try {
    if (!fs::exists(projects_file)) {
      std::cerr << "project not found\n";
      return 1;
    }

    const std::set<std::string> existing_projects = load_top_level_projects(projects_file);
    if (existing_projects.empty()) {
      std::cerr << "project not found\n";
      return 1;
    }
    std::string project_name;
    if (maybe_project_name.has_value()) {
      project_name = *maybe_project_name;
      if (existing_projects.find(project_name) == existing_projects.end()) {
        std::cerr << "project not found\n";
        return 1;
      }
    } else {
      const std::optional<std::string> selected_project = select_project_to_delete(existing_projects);
      if (!selected_project.has_value()) {
        std::cerr << "failed to read project selection\n";
        return 1;
      }
      project_name = *selected_project;
    }

    const std::optional<bool> confirmed = confirm_delete(project_name);
    if (!confirmed.has_value()) {
      std::cerr << "failed to read confirmation\n";
      return 1;
    }
    if (!*confirmed) {
      std::cout << "canceled\n";
      return 1;
    }

    if (!remove_project_block(projects_file, project_name)) {
      std::cerr << "project not found\n";
      return 1;
    }

    std::cout << "deleted project: " << project_name << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap delete failed: " << e.what() << '\n';
    return 1;
  }
}
