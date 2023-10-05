# `gr`

This is a (currently extremely basic) ripgrep / silver searcher / ack /
grep-like tool. It scans for a pattern recursively from the current directory or
the passed paths.

I wrote this primarily to scratch my own itch; it currently has a lot of rough
edges and there's no real reason to use this over any of the alternatives.

The whole thing is written in C++ and it uses Google's RE2 library for regex
matching.

## Installing

Edit the Makefile to suit your platform. Make sure that your `CPATH` and
`LIBRARY_PATH` (or similar on your platform) are set up locally or add them to
the Makefile; as distributed, it assumes that `re2/re2.h` is on your include
path and `libre2` is on your library path.

Then simply:

```
make && sudo make install
```
