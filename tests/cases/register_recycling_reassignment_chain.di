# Register recycling Stage 2 (docs/internal/register-recycling-design.md):
# reassigning an existing local with an arithmetic RHS rewrites the
# producing instruction's own destination in place instead of computing
# into a throwaway temporary and MOVE-ing it in, reclaiming the temp's
# register slot too. Before this, `total = total + 1` repeated N times
# cost about 3 registers *per repetition* (never reclaimed), so a long
# chain like this one used thousands of registers and heap-allocated its
# register file on every call; now the register count stays flat
# regardless of N. Exercises every opcode on the Stage-2 allowlist (see
# opcode_is_rewritable_arithmetic's own comment), the negative case that
# must still use an ordinary MOVE (reassigning from another existing
# local, not a fresh computation), and enough repetitions that a
# reintroduced per-statement leak would time out rather than merely give
# a wrong number.
def chain(seed: Int) -> Int
  total = seed
  i = 0
  while i < 2000
    total = total + 1
    total = total - 1
    total = total * 3
    total = total / 3
    total = total + 1
    total = total % 1000000007
    total = total << 1
    total = total >> 1
    total = total & 0xFFFFFF
    total = total | 0
    total = total ^ 0
    i += 1
  end
  total
end
puts(chain(1))
puts(chain(999))

# The negative case: reassigning from a *different*, already-existing
# local is a bare register reference, not a fresh arithmetic result --
# register_is_named must refuse the rewrite here, or `other` and `total`
# would end up aliased onto one register.
def alias_check(seed: Int) -> Int
  total = seed
  other = seed * 2
  total = other
  other = 999
  total + other
end
puts(alias_check(5))

# A reassignment whose RHS is a comparison must still work correctly
# (EQUAL_INT/etc. are deliberately excluded from the Stage-2 allowlist,
# since compile_binary_op sets compiler->narrowing for them) -- this
# only checks correctness, not that the rewrite was skipped.
def flag_check(seed: Int) -> Bool
  flag = seed > 0
  flag = seed == 0
  flag = seed < 0
  flag
end
puts(flag_check(-5))
puts(flag_check(0))
puts(flag_check(5))
