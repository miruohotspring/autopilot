cmake_minimum_required(VERSION 3.16)

project(autopilot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FTXUI/CMakeLists.txt")
  add_subdirectory(third_party/FTXUI third_party/ftxui-build EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(ftxui)
endif()

add_executable(
  ap
  src/main.cpp
  src/commands/cmd_add.cpp
  src/commands/cmd_briefing.cpp
  src/commands/cmd_delete.cpp
  src/commands/cmd_init.cpp
  src/commands/cmd_list.cpp
  src/commands/cmd_new.cpp
  src/commands/cmd_rm.cpp
  src/commands/delete_ui.cpp
  src/platform/home_dir.cpp
  src/projects/project_paths.cpp
  src/projects/project_store.cpp
)

target_include_directories(ap PRIVATE include)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ap PRIVATE -Wall -Wextra -include tuple)
endif()

find_package(Threads REQUIRED)

target_link_libraries(
  ap
  PRIVATE
    ftxui::component
    ftxui::dom
    ftxui::screen
    Threads::Threads
)

include(GNUInstallDirs)
install(TARGETS ap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
