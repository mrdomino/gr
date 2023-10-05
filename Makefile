CXX=clang++
WFLAGS=-std=c++23 -Wall -Wextra -pedantic
LDFLAGS=-lre2
PREFIX=/usr/local

all: gr

gr: gr.c++ io.c++ job.c++
	$(CXX) $(WFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

clean:
	rm gr

install:
	install -o root -g wheel -m 755 gr $(PREFIX)/bin

.PHONY: all clean install
