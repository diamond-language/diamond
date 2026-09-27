# SHIFT_RIGHT and BITWISE_AND/OR/XOR were never given a disassemble.c
# case at all (found via a full opcode-enum-vs-switch diff while working
# on register recycling Stage 2, docs/internal/register-recycling-design.md
# -- an unrelated program that happened to be the first to --dump-bytecode
# a `>>`/`&`/`|`/`^` expression). Each fell to the switch's own default
# case, which has no idea how many operand bytes an unrecognized opcode
# has and so leaves every instruction after it unreadable for the rest of
# the dump -- a debug-tool-only bug (run_chunk's own dispatch, src/vm.c,
# was never affected; program output was always correct regardless).
puts((12 >> 1) & 15 | 0 ^ 0)
