# Exercise the included method on a user-defined collection, rather than
# the native Array/Hash receiver bridges.
class SumBag
  include Enumerable
  def initialize(values)
    @values = values
  end
  def each(callback)
    @values.each(callback)
  end
end

results = [SumBag.new([]).sum(), SumBag.new([1, 2, 3]).sum(),
           SumBag.new([-5, 2, 3]).sum(), SumBag.new([1, 2.5, 3]).sum()]
values = [4, 5]
bag = SumBag.new(values)
results.push(bag.sum())
values.push(6)
results.push(bag.sum())
results
