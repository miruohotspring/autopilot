CXX ?= c++
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++17
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TARGET ?= ap
SRC ?= main.cpp

.PHONY: all clean install uninstall
.PHONY: test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $<

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

test: $(TARGET)
	./tests/test_ap_init.sh

clean:
	rm -f $(TARGET)
