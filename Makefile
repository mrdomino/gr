CXX=clang++
WFLAGS=-std=c++2b -Wall -Wextra -pedantic
LDFLAGS=-lre2

all: cr

cr: cr.c++
	$(CXX) $(WFLAGS) $(CXXFLAGS) $(LDFLAGS) $< -o $@

clean:
	rm cr

.PHONY: all clean
