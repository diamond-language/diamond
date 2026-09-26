# redefine_method accepts a compile_method callable (replacing an existing
# method with generated code), and self.new() in a class method builds
# whichever class self is.
class Counter
  attr_reader count
  def initialize(start)
    @count = start
  end
  def bump() = @count + 1
  def self.starting_at(start) = self.new(start)
end
class Tens < Counter
end
counter = Counter.starting_at(5)
before = counter.bump()
Counter.redefine_method("bump", Counter.compile_method("bump", [], "self.count() + step", {"step": 10}))
tens = Tens.starting_at(1)
[before, counter.bump(), tens.class(), tens.bump()]
