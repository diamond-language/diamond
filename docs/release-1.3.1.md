# d6 1.3.1

1.3.1 fixes a memory-safety bug in 1.3.0's heredocs. The language is the same as in 1.3.0.

## What was broken in 1.3.0

When a line that opens a heredoc had a syntax error after the opener, as in `x = <<~A $`,
the compiler wrote past the end of one of its tables before reporting the error. The
program was rejected either way, but the stray write could corrupt the compiler's memory
first. The compile fuzzer found it.

## What changed

- The compiler reports the error without the stray write.
- A regression case keeps the sanitizer builds compiling that line.

## Installing

[Getting started](getting-started.md#1-install) compares the ways to install d6. Packages
and tarballs are on the [release page](https://github.com/diamond-language/d6/releases/tag/v1.3.1).
