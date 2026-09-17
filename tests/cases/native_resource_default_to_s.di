# diamond_value_fprint (src/value.c, the CLI's own top-level-result
# auto-print) and builder_format_value/stringify_value (src/vm.c, puts/
# string interpolation) each had one hardcoded "#<Closure>" fallback for
# every object kind neither explicitly handled -- correct only for a
# real Closure, silently wrong for every native resource kind added
# since (Tensor, Channel, Supervisor, Regexp, ...). Found writing
# bench/tensor_matmul.di: `puts(tensor_result)` printed "#<Closure>".
# Both paths now share one canonical per-kind name table
# (diamond_format_value_type, exported from vm.c via vm.h) instead of
# two independently hand-maintained copies.
puts(Channel.new(1))
puts(Regexp.new("x"))
puts(Supervisor.new())

def foo() = 1
puts(foo)

Tensor.zeros(1, 1)
