# Native-only fixture (not mirrored in tests/parser_cases): the self-
# hosted parser has no class-variable support at all yet, in any
# assignment form, so there's nothing to differential-test against here.
class Counter
  def self.reset()
    @@count = 0
  end
  def initialize()
    @@count += 1
  end
  def self.count()
    @@count
  end
end
Counter.reset()
Counter.new()
Counter.new()
Counter.new()
Counter.count()
