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

file(GLOB_RECURSE AUTOPILOT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(SORT AUTOPILOT_SOURCES)

add_executable(ap ${AUTOPILOT_SOURCES})

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
