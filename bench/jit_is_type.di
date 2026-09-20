# JIT Phase 14 (docs/internal/jit-design.md): DIAMOND_OP_IS_TYPE now
# has JIT codegen at all. Before this phase, `classify` below made the
# whole enclosing loop bail JIT eligibility outright -- not a narrowing
# proof gap, a complete absence of any compile_body case for the opcode.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs --
# DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_is_type.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_is_type.di
def classify(x: Int | String) -> Int
  if x is Int
    1
  else
    2
  end
end

def run_loop(iterations) -> Int
  total = 0
  i = 0
  while i < iterations
    total = total + classify(i)
    i = i + 1
  end
  total
end

puts(run_loop(3000000))
