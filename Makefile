WFLAGS=-std=c++23 -Wall -Wextra -pedantic
LDFLAGS=-lre2
PREFIX=/usr/local

OBJS=gr.o circle_queue.o io.o job.o opts.o

all: gr

%.o: %.c++
	$(CXX) $(WFLAGS) $(CXXFLAGS) -c $< -o $@

io.o: io.h
job.o: job.h
opts.o: opts.h
circle_queue.o: circle_queue.h
gr.o: circle_queue.h io.h job.h opts.h

gr: $(OBJS)
	$(CXX) $(WFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

clean:
	-rm $(OBJS)

install:
	install -o root -g wheel -m 755 gr $(PREFIX)/bin

.PHONY: all clean install
