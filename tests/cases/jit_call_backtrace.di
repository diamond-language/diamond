# An error raised below compiled callers still names each of them, with the line
# of the call, in the same order as the interpreter's trace.
def inner(x)
  raise ArgumentError.new("boom") if x > 2
  x
end

def middle(x)
  inner(x + 1)
end

def outer(x)
  middle(x + 1)
end

puts(outer(-5))
begin
  outer(1)
rescue error: ArgumentError
  puts(error.backtrace().length())
end
outer(1)
