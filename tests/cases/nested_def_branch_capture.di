# Regression test for a fixed compiler bug: a nested `def` lexically
# inside one branch of an `if`/`else` blanket-captures every local
# already in scope (boxing each one via BOX_LOCAL right before the
# CLOSURE it creates), which used to mark those locals `captured` for
# the rest of the enclosing function's compilation -- including reads
# reached only through a sibling branch that never executes that
# BOX_LOCAL at runtime. Each case below exercises one of the four call
# sites that read/write a `captured` local (a plain reference, a call
# through it, an indexed-assignment receiver, and a plain reassignment)
# from such a sibling branch.

def read_case(condition, value)
  if condition
    def noop_read()
      1
    end
  else
    value
  end
end

def call_case(condition, fn)
  if condition
    def noop_call()
      1
    end
  else
    fn()
  end
end

def index_case(condition, arr)
  if condition
    def noop_index()
      1
    end
  else
    arr[0] = 99
    arr
  end
end

def write_case(condition, value)
  if condition
    def noop_write()
      1
    end
  else
    value = value + 1
    value
  end
end

def call_fn()
  42
end

[
  read_case(false, "hello"),
  call_case(false, call_fn),
  index_case(false, [1, 2, 3]),
  write_case(false, 10),
]
