# Register recycling Stage 1 let a new local adopt its right-hand side's
# own register as its home. A `nil` literal emits no instruction (its
# register is assumed to stay at the zero-initialised nil), so adopting it
# meant `seen = nil` at the top of a loop body never reset `seen`: once the
# body assigned `seen` later, the next iteration started from that stale
# value. Covers a plain local, a conditionally-set local, and a local a
# block captures, in both a `while` body and a block body.
def plain()
  i = 0
  while i < 3
    seen = nil
    puts(seen == nil)
    seen = i + 10
    i += 1
  end
end

def conditional()
  i = 0
  while i < 3
    found = nil
    puts(found == nil)
    found = 7 if i == 0
    i += 1
  end
end

def captured()
  i = 0
  while i < 3
    seen = nil
    puts(seen == nil)
    [i].each() do |x| seen = x + 10 end
    i += 1
  end
end

def in_block()
  (0...3).each() do |i|
    seen = nil
    puts(seen == nil)
    seen = i
  end
end

plain()
conditional()
captured()
in_block()
nil
