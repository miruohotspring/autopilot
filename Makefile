CXX ?= c++
FTXUI_VERSION ?= v6.1.9
FTXUI_DIR ?= third_party/FTXUI
FTXUI_BUILD_DIR ?= build/ftxui
FTXUI_CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-include\ tuple -DFTXUI_BUILD_DOCS=OFF -DFTXUI_BUILD_EXAMPLES=OFF -DFTXUI_BUILD_TESTS=OFF -DFTXUI_ENABLE_INSTALL=OFF -DFTXUI_QUIET=ON

CPPFLAGS ?=
CPPFLAGS += -Iinclude -isystem $(FTXUI_DIR)/include
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
LDFLAGS ?=
LDLIBS ?=
LDLIBS += -pthread
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TARGET ?= ap
SRC_DIR ?= src
SRC ?= $(sort $(shell find $(SRC_DIR) -type f -name '*.cpp'))
FTXUI_CONFIG_STAMP := $(FTXUI_BUILD_DIR)/.configured
FTXUI_LIBS := $(FTXUI_BUILD_DIR)/libftxui-component.a $(FTXUI_BUILD_DIR)/libftxui-dom.a $(FTXUI_BUILD_DIR)/libftxui-screen.a

.PHONY: all clean install uninstall
.PHONY: test

all: $(TARGET)

$(FTXUI_DIR)/CMakeLists.txt:
	mkdir -p $(dir $(FTXUI_DIR))
	git clone --depth 1 --branch $(FTXUI_VERSION) https://github.com/ArthurSonzogni/FTXUI.git $(FTXUI_DIR)

$(FTXUI_CONFIG_STAMP): $(FTXUI_DIR)/CMakeLists.txt
	cmake -S $(FTXUI_DIR) -B $(FTXUI_BUILD_DIR) $(FTXUI_CMAKE_FLAGS)
	touch $(FTXUI_CONFIG_STAMP)

$(FTXUI_LIBS): $(FTXUI_CONFIG_STAMP)
	cmake --build $(FTXUI_BUILD_DIR) --target component

$(TARGET): $(SRC) $(FTXUI_LIBS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SRC) $(FTXUI_LIBS) $(LDFLAGS) $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

test: $(TARGET)
	bash ./tests/run_all_tests.sh

clean:
	rm -f $(TARGET)
