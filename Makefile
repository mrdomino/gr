WFLAGS=-std=c++2b -Wall -Wextra -pedantic

all: cr

cr: cr.c++
	$(CXX) $(WFLAGS) $(CXXFLAGS) $< -o $@

clean:
	rm cr

.PHONY: all clean
