class Factory
  def self.reference() = Factory.collect
  def self.collect(*values) = values.sum()
end
reference = Factory.reference()
puts(reference(1, 2, 3))
