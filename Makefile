CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O2
PREFIX ?= /usr/local
DESTDIR ?=
INCLUDEDIR ?= $(PREFIX)/include

EXAMPLE := build/disabled

.PHONY: all check install clean

all: check

check: $(EXAMPLE)
	./$(EXAMPLE)

$(EXAMPLE): examples/disabled.cpp peek.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++20 -pthread -I. $< -o $@

install:
	mkdir -p "$(DESTDIR)$(INCLUDEDIR)" "$(DESTDIR)$(PREFIX)/share/licenses/peek"
	cp peek.hpp "$(DESTDIR)$(INCLUDEDIR)/peek.hpp"
	cp LICENSE "$(DESTDIR)$(PREFIX)/share/licenses/peek/LICENSE"

clean:
	rm -rf build
