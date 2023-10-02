CXX=clang++
WFLAGS=-std=c++2b -Wall -Wextra -pedantic
LDFLAGS=-lre2
PREFIX=/usr/local

all: cr

cr: cr.c++
	$(CXX) $(WFLAGS) $(CXXFLAGS) $(LDFLAGS) $< -o $@

clean:
	rm cr

install:
	install -o root -g wheel -m 755 cr $(PREFIX)/bin

.PHONY: all clean install
