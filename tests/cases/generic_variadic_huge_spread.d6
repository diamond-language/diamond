# A generic function infers its type variables from each argument's declared
# parameter type before the call's arity is checked. That loop used to run once per
# *argument*, indexing a 32-entry array, so a spread call with a huge count read
# far past it (heap-buffer-overflow under ASan, a crash with enough arguments).
# The call is rejected afterwards; this must reject it without the bad read.
def collect[T](first: T, *rest)
  rest.length()
end

begin
  collect(*(0...100000).to_a())
  "accepted"
rescue error: StandardError
  "rescued: " + error.message()
end
