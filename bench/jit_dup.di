# JIT Phase 4 (docs/internal/jit-design.md): DIAMOND_OP_INVOKE support
# for the three receiver-kind-agnostic pseudo-methods `dup`/`freeze`/
# `frozen?`, none of which can ever invoke arbitrary user code -- this
# isolates just `dup` (real per-type native methods and Instance
# dispatch remain entirely unsupported by the JIT). `make_copy` is its
# own function specifically so it has no has_called-setting opcode
# before its one `dup()` call in the SAME compiled function -- a `dup`
# call reached after an ordinary loop comparison in the same function
# does not compile (see jit-design.md's own Phase 4 note); calling it
# repeatedly from a separate driver loop, as skindicate's real
# ActiveRecord::Model#initialize is (packages/active_record/lib/
# active_record/model.di), sidesteps that entirely.
#
# Not part of the automatic default-vs-quicken sweep bench/run.sh runs
# -- DIAMOND_JIT is a separate opt-in tier, so compare by hand:
#   ./build/diamond bench/jit_dup.di
#   DIAMOND_JIT=1 ./build/diamond bench/jit_dup.di
# Expect a modest win (Phase 4's own bench/object_hydration.di
# measurement was ~8-10% end to end) -- most of the per-call cost is
# inside diamond_jit_dup's own Hash-copy work either way; this tier
# removes bytecode dispatch overhead around that call, not the copy
# itself.
def make_copy(h)
  h.dup()
end

def run()
  h = {"a": 1, "b": 2, "c": 3, "d": 4, "e": 5}
  index = 0
  total = 0
  while index < 500000
    copy = make_copy(h)
    total = total + copy.length()
    index = index + 1
  end
  total
end
run()
