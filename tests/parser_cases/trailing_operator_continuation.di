# Regression fixture for a self-hosted-parser-only bug found while
# implementing compound assignment: src/compiler.c's parse_precedence
# skips a newline right after an infix operator (fixes `x = 1 +\n 2`),
# but selfhost/parser.di's own mirror never got that fix -- latent until
# something (here, compound_assignment_token?'s own multi-line || chain)
# finally exercised the pattern. Covers the operators most likely to be
# written this way: arithmetic, boolean, and comparison.
a = 1 +
  2
b = 3 -
  1
c = true ||
  false
d = false &&
  true
e = 5 >
  3
[a, b, c, d, e]
