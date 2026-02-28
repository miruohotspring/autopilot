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
    if (state.active) {
      return entry | ftxui::color(ftxui::Color::Blue);
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

}  // namespace

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
