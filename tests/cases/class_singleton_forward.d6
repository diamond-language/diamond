class Factory
  BASE = Factory.default_base()
  def value() = Factory.later(BASE)
  def self.first() = Factory.later()
  def self.reference() = Factory.later
  def self.default_base() = 40
  def self.later(amount: Int = 2) -> Int = amount + 2
  def self.even(value)
    if value == 0 then true else Factory.odd(value - 1) end
  end
  def self.odd(value)
    if value == 0 then false else Factory.even(value - 1) end
  end
end
puts(Factory::BASE)
puts(Factory.new().value())
puts(Factory.first())
reference = Factory.reference()
puts(reference(5))
puts(Factory.even(6))
puts(Factory.odd(6))
class Factory
  def self.reopened() = Factory.added(3, 4)
  def self.added(*values) = values.sum()
end
puts(Factory.reopened())
