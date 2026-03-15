#include "autopilot/commands/delete_ui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

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

bool is_interactive_terminal() {
  return ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO);
}

std::optional<std::string> select_value_with_prompt(
    const std::vector<std::string>& value_list,
    const std::string& heading,
    const std::string& number_prompt) {
  if (value_list.empty()) {
    return std::nullopt;
  }

  std::cout << heading << '\n';
  for (std::size_t i = 0; i < value_list.size(); ++i) {
    std::cout << "  " << (i + 1) << ") " << value_list[i] << '\n';
  }

  while (true) {
    std::cout << number_prompt;
    std::cout.flush();

    std::string answer;
    if (!std::getline(std::cin, answer)) {
      return std::nullopt;
    }

    const std::string normalized = trim_ascii_whitespace(answer);
    try {
      std::size_t pos = 0;
      const unsigned long selected = std::stoul(normalized, &pos, 10);
      if (pos == normalized.size() && selected >= 1 && selected <= value_list.size()) {
        return value_list[selected - 1];
      }
    } catch (const std::exception&) {
    }

    std::cout << "Please enter a number between 1 and " << value_list.size() << ".\n";
  }
}

std::optional<std::string>
select_value_with_tui(std::vector<std::string> value_list, const std::string& prompt) {
  if (value_list.empty()) {
    return std::nullopt;
  }

  std::size_t max_width = prompt.size();
  for (const std::string& value : value_list) {
    max_width = std::max(max_width, value.size() + 3U);
  }
  const auto terminal_size = ftxui::Terminal::Size();
  const int ui_width = std::max(1, std::min<int>(static_cast<int>(max_width), terminal_size.dimx));
  const int ui_height =
      std::max(2, std::min<int>(static_cast<int>(value_list.size()) + 1, terminal_size.dimy));

  int selected = 0;
  bool confirmed = false;
  auto screen = ftxui::ScreenInteractive::FixedSize(ui_width, ui_height);

  ftxui::MenuOption menu_option;
  menu_option.entries_option.transform = [](const ftxui::EntryState& state) {
    ftxui::Element entry = ftxui::text(state.label);
    if (state.active) {
      return entry | ftxui::color(ftxui::Color::Blue);
    }
    return entry | ftxui::dim;
  };
  menu_option.on_enter = [&] {
    confirmed = true;
    screen.ExitLoopClosure()();
  };

  ftxui::Component menu = ftxui::Menu(&value_list, &selected, menu_option);
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
  return value_list[static_cast<std::size_t>(selected)];
}

std::optional<std::string> select_project_with_ui(
    const std::set<std::string>& projects,
    const std::string& heading,
    const std::string& tui_prompt,
    const std::string& number_prompt) {
  const std::vector<std::string> project_list(projects.begin(), projects.end());
  if (is_interactive_terminal()) {
    return select_value_with_tui(project_list, tui_prompt);
  }
  return select_value_with_prompt(project_list, heading, number_prompt);
}

std::optional<std::string> select_path_with_ui(
    const std::vector<std::string>& paths,
    const std::string& heading,
    const std::string& tui_prompt,
    const std::string& number_prompt) {
  if (is_interactive_terminal()) {
    return select_value_with_tui(paths, tui_prompt);
  }
  return select_value_with_prompt(paths, heading, number_prompt);
}

std::optional<bool> confirm_yes_no(const std::string& prompt) {
  while (true) {
    std::cout << prompt;
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

} // namespace

std::optional<std::string> select_project_to_delete(const std::set<std::string>& projects) {
  return select_project_with_ui(
      projects,
      "Select project to delete:",
      "? Select project to delete (Use arrow keys or j/k)",
      "Enter number to delete: ");
}

std::optional<std::string> select_project_to_add_path(const std::set<std::string>& projects) {
  return select_project_with_ui(
      projects,
      "Select project to add path:",
      "? Select project to add path (Use arrow keys or j/k)",
      "Enter number to add path: ");
}

std::optional<std::string> select_project_to_add_task(const std::set<std::string>& projects) {
  return select_project_with_ui(
      projects,
      "Select project to add task:",
      "? Select project to add task (Use arrow keys or j/k)",
      "Enter number to add task: ");
}

std::optional<std::string> select_project_to_remove_path(const std::set<std::string>& projects) {
  return select_project_with_ui(
      projects,
      "Select project to remove path:",
      "? Select project to remove path (Use arrow keys or j/k)",
      "Enter number to remove path: ");
}

std::optional<std::string> select_project_to_start(const std::set<std::string>& projects) {
  return select_project_with_ui(
      projects,
      "Select project to start:",
      "? Select project to start (Use arrow keys or j/k)",
      "Enter number to start: ");
}

std::optional<std::string> select_path_to_remove(const std::vector<std::string>& paths) {
  return select_path_with_ui(
      paths,
      "Select path to remove:",
      "? Select path to remove (Use arrow keys or j/k)",
      "Enter number to remove: ");
}

std::optional<bool> confirm_delete(const std::string& project_name) {
  return confirm_yes_no("Delete project '" + project_name + "'? [y/n]: ");
}

std::optional<bool> confirm_remove_path(const std::string& path_value) {
  return confirm_yes_no("Remove path '" + path_value + "'? [y/n]: ");
}
