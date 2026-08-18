# A block on a direct (non-method) function call, exercising parse_call's
# own direct-call path rather than parse_invoke's method-call path.
def apply(x, callback)
  callback(x)
end
apply(5) do |n|
  n * n
end
