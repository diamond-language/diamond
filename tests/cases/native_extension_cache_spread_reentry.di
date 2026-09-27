# A native receiver's spread call (`arr.method(*args)`) re-enters the
# ordinary INVOKE dispatch through a synthetic DiamondChunk whose bytecode
# lives in a C stack buffer local to that one case -- reused at the same
# stack address on every spread call in the program, unlike a real call
# site's own bytecode address. The extension-lookup cache (added the same
# session, see the commit after "About 120 common Ruby methods") first
# keyed on that address and so had two *different* spread-called methods
# collide onto one cache entry: `.large?(*[3])` got back the function
# resolved for an earlier `.map(*[double])` at the same reused address,
# and ran `enumerable_map` with an Int in place of a Callable. Keying on
# the method name's own (stable, per-function, never-reallocated) string
# constant fixed it. This interleaves several different spread-called
# methods, across Array/Hash/String receivers, enough times to catch any
# reintroduced collision (a fresh cache slot only gets exercised as such
# on a repeat visit).
def flag(v)
  v > 2
end
def doubler(v)
  v * 2
end
def array_large(values: Array, minimum)
  values.length() >= minimum
end

results = []
100.times() do |i|
  results.push([1, 2, 3].map(*[doubler]).join(","))
  results.push([1, 2, 3].select(*[flag]).length())
  results.push({"x": 1, "y": 2}.fetch(*["x", 0]))
  results.push("hello world".slice(*[1, 3]))
  results.push([1, 2, 3].first(*[2]).join(","))
  results.push([3, 1, 2].large?(*[2]))
end
puts(results.uniq().to_s())
