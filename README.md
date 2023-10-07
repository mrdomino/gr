# `gr`

This is a (currently extremely basic) ripgrep / silver searcher / ack
/ grep-like tool. It scans for a pattern recursively from the current
directory or the passed paths.

It's written in C++ and uses Google's RE2 library for regex matching.

It's written to the defaults I'd prefer — e.g. no colors, but uses bold
when outputting to a tty. Some light effort is made to be UTF8-aware
(and RE2 is itself UTF8-aware), but there are probably bugs.

## Installing

Edit the Makefile to suit your platform. Make sure that your `CPATH` and
`LIBRARY_PATH` (or equivalent on your platform) are set up locally or
add them to the Makefile; as distributed, it assumes that `re2/re2.h` is
on your include path and `libre2` is on your library path.

Then simply:

```
make && sudo make install
```
