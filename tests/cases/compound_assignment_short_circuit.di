# ||=/&&= must genuinely short-circuit -- the RHS is never evaluated
# (and never re-assigned) when the existing value already decides the
# outcome. `calls` counts how many times the RHS actually ran.
calls = [0]
def bump(calls)
  calls[0] = calls[0] + 1
  99
end

a = 5
a ||= bump(calls)   # a already truthy: RHS not evaluated, a stays 5

b = nil
b ||= bump(calls)   # b falsy: RHS evaluated, b becomes 99

c = false
c &&= bump(calls)   # c already falsy: RHS not evaluated, c stays false

d = 7
d &&= bump(calls)   # d truthy: RHS evaluated, d becomes 99

[a, b, c, d, calls[0]]
