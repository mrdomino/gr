CXX=clang++
WFLAGS=-std=c++2b -Wall -Wextra -pedantic
LDFLAGS=-lre2
PREFIX=/usr/local

all: gr

gr: gr.c++
	$(CXX) $(WFLAGS) $(CXXFLAGS) $(LDFLAGS) $< -o $@

clean:
	rm gr

install:
	install -o root -g wheel -m 755 gr $(PREFIX)/bin

.PHONY: all clean install
