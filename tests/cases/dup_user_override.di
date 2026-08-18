# A class's own `def dup` must win over the built-in shallow-copy
# fallback -- checked via lookup_method before the fallback ever runs.
class Special
  def initialize(x)
    @x = x
  end
  def dup()
    Special.new(999)
  end
  def x() = @x
end
Special.new(1).dup().x()
