# A register whose sole *provable* write is a known-return-type INVOKE
# still bails once it's written a second time anywhere else in the
# function -- same whole-body "exactly one write" rule Phase 9/10/11's
# own reassignment tests already cover, exercised here for the new
# INVOKE terminal specifically.
class Widget
  def initialize(n)
    @n = n
  end
  def double() -> Int = @n * 2
end

class Factory
  def make() -> Widget = Widget.new(21)
end

def run(f: Factory, n) -> Int
  x = f.make()
  total = 0
  i = 0
  while i < n
    total = total + x.double()
    if i == 3
      x = f.make()
    end
    i = i + 1
  end
  total
end

puts(run(Factory.new(), 6))
