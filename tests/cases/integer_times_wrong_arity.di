# A zero-param block passed where `Callable[1]` is required is a
# structural-type mismatch, same as any other Callable[1] consumer
# (.select, .map, ...) -- see block_wrong_arity.di.
3.times() do
  1
end
