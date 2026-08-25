class Base
  def initialize()
    @data = {}
  end
  def [](key) = @data[key]
  def []=(key, value)
    @data[key] = value
  end
end
class Child < Base
end
child = Child.new()
child["a"] = 1
child["a"]
