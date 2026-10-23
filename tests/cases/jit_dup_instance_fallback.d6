# A class that overrides `dup` itself is the one case Phase 6's own
# Instance-dup support still can't handle inline (real method dispatch,
# not attempted) -- diamond_jit_dup's own lookup_method check correctly
# detects the override and falls back to full interpretation instead of
# running its own default copy logic.
class Widget
  def initialize(n)
    @n = n
  end
  def dup() = Widget.new(999)
  def n() = @n
end

def dup_it(x) = x.dup()

w = Widget.new(42)
d = dup_it(w)
puts(d.n())
