# A zero-param block passed where `Callable[1]` is required is a
# structural-type mismatch at the call site, not a runtime arity crash --
# Diamond has no Ruby-style silent-nil-for-missing-params leniency.
[1, 2, 3].select() do
  true
end
