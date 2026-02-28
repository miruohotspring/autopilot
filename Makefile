CXX ?= c++
CPPFLAGS ?= -Iinclude
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TARGET ?= ap
SRC ?= src/main.cpp src/platform/home_dir.cpp src/commands/cmd_init.cpp src/commands/cmd_new.cpp

.PHONY: all clean install uninstall
.PHONY: test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

test: $(TARGET)
	bash ./tests/run_all_tests.sh

clean:
	rm -f $(TARGET)
