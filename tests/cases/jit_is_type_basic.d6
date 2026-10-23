# JIT Phase 14 (docs/internal/jit-design.md): DIAMOND_OP_IS_TYPE gets JIT
# codegen for the first time. Before this, ANY function containing an
# `is` check -- however simple, no receiver chaining involved at all --
# bailed the whole function outright, since compile_body's dispatch
# switch had no case for it and fell straight to `default: jc->bailed =
# true`. `classify` here is the simplest possible reproduction: a
# declared-union parameter tested with a single `is`, no method calls on
# either side of the branch.
def classify(x: Int | String) -> Int
  if x is Int
    1
  else
    2
  end
end

total = 0
i = 0
while i < 5
  total = total + classify(1) + classify("s")
  i = i + 1
end
puts(total)
