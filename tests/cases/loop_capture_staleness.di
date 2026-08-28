# A loop body compiles exactly once; every iteration after the first
# reaches it via a jump back to that same already-compiled bytecode. If a
# def/closure captures a local, capturing it boxes that local's register
# in place -- any reference to the local compiled *before* the capture
# was discovered (an ordinary raw register read, since the compiler
# didn't know yet) goes stale: correct on the first iteration (box hasn't
# happened yet), a type error on every iteration after. Fixed by scanning
# a loop's own body for a def/closure before compiling anything, and if
# found, marking every already-visible local -- and every local declared
# fresh from then on -- captured from the start, so all of this already
# goes through the existing box-aware codegen. See docs/roadmap.md's
# "Open design decisions" section.

def call_it(cb)
  cb()
end

# The original case: a pre-existing local referenced in the while
# condition itself.
def while_condition_case()
  i = 0
  results = []
  while i < 3
    def show()
      i
    end
    results.push(show())
    i += 1
  end
  results
end

# No separate while-condition at all -- proves the fix isn't
# condition-specific. The stale reference here is an ordinary body
# statement (`break if`), not a loop condition expression.
def loop_break_case()
  i = 0
  results = []
  loop do
    break if i >= 3
    def show()
      i
    end
    results.push(show())
    i += 1
  end
  results
end

# A local declared *fresh inside* the loop body, captured by a closure
# later in the same body -- independent of any pre-existing outer local.
def fresh_local_case()
  results = []
  while results.length() < 3
    x = results.length() * 10
    def show()
      x
    end
    results.push(show())
  end
  results
end

# break/next/redo still behave correctly inside a capturing loop --
# confirms the fix needed no changes to loop-control jump targets.
def loop_control_case()
  i = 0
  results = []
  redo_done = false
  while i < 5
    def show()
      i
    end
    i += 1
    if i == 2
      next
    end
    if i == 3 && !redo_done
      redo_done = true
      redo
    end
    if i >= 4
      break
    end
    results.push(show())
  end
  results
end

# Nested loops: only the *inner* loop captures.
def inner_captures_case()
  results = []
  outer = 0
  while outer < 2
    inner = 0
    while inner < 2
      def show()
        outer * 10 + inner
      end
      results.push(show())
      inner += 1
    end
    outer += 1
  end
  results
end

# Nested loops: only the *outer* loop captures (the inner loop only
# reads a plain, uncaptured local) -- confirms loop_captures_pending's
# inheritance direction (outer -> inner), not just presence at one level.
def outer_captures_case()
  results = []
  outer = 0
  while outer < 2
    def show()
      outer
    end
    inner = 0
    while inner < 2
      results.push(show() * 10 + inner)
      inner += 1
    end
    outer += 1
  end
  results
end

# `rescue e` binds the caught value via a direct locals[] registration
# that bypasses define_local, same as every case above goes through it.
# Fixed defensively to also consult loop_captures_pending, even though
# no observable divergence was found for this specific site: the VM
# rewrites the exception register fresh on every catch, which happens
# to mask staleness here. Kept as regression/coverage rather than a
# bug demonstration.
def rescue_binding_case(n)
  results = []
  i = 0
  while i < n
    begin
      raise "boom"
    rescue e
      def grab_it()
        e
      end
      results.push(grab_it())
    end
    i += 1
  end
  results
end

# A `def`'s own name is registered as a Callable-value local in the
# *outer* scope right after it's compiled -- also bypasses define_local.
# Fixed defensively for the same reason as above: here that name
# (`show`) is itself captured by a second def declared later in the
# same loop body.
def def_as_local_capture_case(n)
  results = []
  i = 0
  while i < n
    def show()
      i
    end
    def call_show()
      show()
    end
    results.push(call_show())
    i += 1
  end
  results
end

puts(while_condition_case())
puts(loop_break_case())
puts(fresh_local_case())
puts(loop_control_case())
puts(inner_captures_case())
puts(outer_captures_case())
puts(rescue_binding_case(3))
puts(def_as_local_capture_case(3))
