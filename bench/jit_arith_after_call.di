# JIT Phase 3 (docs/internal/jit-design.md): before this phase,
# ADD_INT/LESS_INT reached after a call-capable opcode already ran in
# the same compiled function (here, the Hash INDEX_GET on "value") was
# rejected outright at JIT compile time -- jc->has_called had no safe
# way to resume an arithmetic edge case once a real call might already
# have happened, so this exact function would have been permanently
# JIT-ineligible (0 compiled functions, no matter how hot), falling
# back to full interpretation forever. Phase 3 gave ADD_INT/LESS_INT a
# resumable trampoline instead, closing that gap -- this is the same
# shape tests/cases/jit_int_arith_after_index_get.di regression-tests,
# sized for timing rather than correctness. `n: Int` is required for
# LESS_INT to be selected at compile time at all (an untyped parameter
# can't be proven Int statically, so `index < n` would otherwise stay
# generic LESS, which the JIT has never supported at any phase).
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs
# -- DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_arith_after_call.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_arith_after_call.di
def hydrate(hash, n: Int)
  index = 0
  count = 0
  while index < n
    v = hash["value"]
    count = count + 1
    index = index + 1
  end
  count
end

def run()
  h = {"value": 7}
  hydrate(h, 3000000)
end
run()
